#include <errno.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "avk_sync.h"

bool avk_sync_init(struct avk_sync *sync, struct avk_device *dev,
		const char *name) {
	memset(sync, 0, sizeof(*sync));
	sync->dev = dev;

	/* Both halves, or neither. A device that can export but not import can
	 * hand a frame to KMS and then has no way to wait for KMS to give the
	 * buffer back, which is half a synchronisation scheme and worse than
	 * knowing you have none. */
	if (!dev->caps.semaphore_sync_fd_export ||
			!dev->caps.semaphore_sync_fd_import) {
		avk_log(AVK_ERROR, "this device cannot %s a binary semaphore as a "
			"sync_file, so a finished frame cannot be handed to the display "
			"with a fence attached",
			!dev->caps.semaphore_sync_fd_export ? "export" : "import");
		return false;
	}
	if (dev->api.vkGetSemaphoreFdKHR == NULL ||
			dev->api.vkImportSemaphoreFdKHR == NULL) {
		avk_log(AVK_ERROR, "VK_KHR_external_semaphore_fd entry points are "
			"missing; presentation synchronisation is not possible");
		return false;
	}

	VkExportSemaphoreCreateInfo export_info = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VkSemaphoreCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &export_info,
	};
	VkResult res = vkCreateSemaphore(dev->dev, &info, NULL, &sync->export_sem);
	if (res != VK_SUCCESS) {
		return avk_check(res, "vkCreateSemaphore (sync_file export)");
	}
	AVK_LIVE_INC(dev, semaphores);
	avk_device_name_object(dev, VK_OBJECT_TYPE_SEMAPHORE,
		(uint64_t)sync->export_sem, "avk %s present fence", name);

	/* The wait slots are plain binary semaphores. They are never signalled by
	 * a submission -- a sync_file import replaces their payload wholesale --
	 * so they need no export flag and no initial value. */
	for (uint32_t i = 0; i < AVK_SYNC_WAIT_SLOTS; i++) {
		VkSemaphoreCreateInfo wait_info = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};
		res = vkCreateSemaphore(dev->dev, &wait_info, NULL,
			&sync->waits[i].semaphore);
		if (res != VK_SUCCESS) {
			avk_check(res, "vkCreateSemaphore (sync_file wait)");
			avk_sync_finish(sync);
			return false;
		}
		AVK_LIVE_INC(dev, semaphores);
		avk_device_name_object(dev, VK_OBJECT_TYPE_SEMAPHORE,
			(uint64_t)sync->waits[i].semaphore, "avk %s wait slot %u", name, i);
	}

	return true;
}

void avk_sync_finish(struct avk_sync *sync) {
	if (sync->dev == NULL) {
		return;
	}
	if (sync->export_sem != VK_NULL_HANDLE) {
		vkDestroySemaphore(sync->dev->dev, sync->export_sem, NULL);
		AVK_LIVE_DEC(sync->dev, semaphores);
		sync->export_sem = VK_NULL_HANDLE;
	}
	for (uint32_t i = 0; i < AVK_SYNC_WAIT_SLOTS; i++) {
		if (sync->waits[i].semaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(sync->dev->dev, sync->waits[i].semaphore, NULL);
			AVK_LIVE_DEC(sync->dev, semaphores);
			sync->waits[i].semaphore = VK_NULL_HANDLE;
		}
	}
	sync->dev = NULL;
}

bool avk_sync_export_sync_file(struct avk_sync *sync, int *out_fd) {
	*out_fd = -1;
	VkSemaphoreGetFdInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		.semaphore = sync->export_sem,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	int fd = -1;
	VkResult res = sync->dev->api.vkGetSemaphoreFdKHR(sync->dev->dev, &info,
		&fd);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkGetSemaphoreFdKHR");
		sync->export_fails++;
		return false;
	}
	/* An fd of -1 with VK_SUCCESS is the driver saying the work is already
	 * done, which is a legal answer and not an error. */
	sync->exports++;
	*out_fd = fd;
	return true;
}

VkSemaphore avk_sync_import_sync_file(struct avk_sync *sync,
		int sync_file_fd) {
	if (sync_file_fd < 0) {
		return VK_NULL_HANDLE;
	}

	struct avk_sync_slot *slot = &sync->waits[sync->next_wait];
	sync->next_wait = (sync->next_wait + 1) % AVK_SYNC_WAIT_SLOTS;

	/* Has the submission that was supposed to consume this slot's last payload
	 * actually run? It always should have -- the command ring is shorter than
	 * this list -- and the point of checking is that if it ever has not, the
	 * symptom is a wait on a stale fence, which looks like a rendering
	 * corruption rather than like a synchronisation bug.
	 *
	 * `consumed_at` is recorded as the next timeline value at import time,
	 * which is the frame's submission unless something else (an upload) slips
	 * a submission in between. That makes the check slightly optimistic: it
	 * can believe a slot is free one or two points early. It is a backstop for
	 * a mistake, not a proof of correctness, and it is written down as such
	 * rather than presented as one. */
	if (slot->consumed_at != 0 &&
			avk_device_timeline_value(sync->dev) < slot->consumed_at) {
		sync->reuse_hazards++;
		avk_log(AVK_ERROR, "sync wait slot reused while its previous payload "
			"was still pending (timeline %" PRIu64 " < %" PRIu64 "); this is a "
			"bug in the frame path, not in the driver",
			avk_device_timeline_value(sync->dev), slot->consumed_at);
	}

	VkImportSemaphoreFdInfoKHR import = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
		.semaphore = slot->semaphore,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
		/* TEMPORARY: the payload belongs to this one wait and is gone
		 * afterwards, which is exactly the lifetime a frame's acquire fence
		 * has. A permanent import would leave the semaphore holding a fence
		 * from three frames ago. */
		.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
		.fd = sync_file_fd,
	};
	VkResult res = sync->dev->api.vkImportSemaphoreFdKHR(sync->dev->dev,
		&import);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkImportSemaphoreFdKHR");
		sync->import_fails++;
		close(sync_file_fd);
		return VK_NULL_HANDLE;
	}
	/* On success the driver owns the fd. */

	/* The next submission is the one that will wait on this. */
	slot->consumed_at = sync->dev->timeline_next;
	sync->imports++;
	return slot->semaphore;
}

bool avk_sync_dmabuf_attach(int dmabuf_fd, uint32_t flags, int sync_file_fd) {
	if (sync_file_fd < 0) {
		return false;
	}
	struct dma_buf_import_sync_file data = {
		.flags = flags,
		.fd = sync_file_fd,
	};
	int ret;
	do {
		ret = ioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &data);
	} while (ret != 0 && (errno == EINTR || errno == EAGAIN));
	int err = errno;
	close(sync_file_fd);
	if (ret != 0) {
		avk_log(err == ENOTTY ? AVK_WARN : AVK_ERROR,
			"DMA_BUF_IOCTL_IMPORT_SYNC_FILE failed: %s%s", strerror(err),
			err == ENOTTY ? " (kernel is older than 5.20)" : "");
		errno = err;
		return false;
	}
	return true;
}

bool avk_sync_dmabuf_fences(int dmabuf_fd, uint32_t flags, int *out_fd) {
	*out_fd = -1;
	struct dma_buf_export_sync_file data = {
		.flags = flags,
		.fd = -1,
	};
	int ret;
	do {
		ret = ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &data);
	} while (ret != 0 && (errno == EINTR || errno == EAGAIN));
	if (ret != 0) {
		int err = errno;
		avk_log(err == ENOTTY ? AVK_WARN : AVK_ERROR,
			"DMA_BUF_IOCTL_EXPORT_SYNC_FILE failed: %s", strerror(err));
		errno = err;
		return false;
	}
	*out_fd = data.fd;
	return true;
}

void avk_sync_log_stats(const struct avk_sync *sync, const char *name) {
	avk_log(AVK_INFO, "sync[%s]: exports=%" PRIu64 " imports=%" PRIu64
		" export_fails=%" PRIu64 " import_fails=%" PRIu64
		" reuse_hazards=%" PRIu64, name, sync->exports, sync->imports,
		sync->export_fails, sync->import_fails, sync->reuse_hazards);
}
