#include "background-effect.h"
#include "modern.h"
/* M13: the presentation class, after modern.h (which reads wp-content-type)
 * and before tearing.h (which used to read it directly). */
#include "../present/az_present_intent.h"
#include "dwl-ipc.h"
#include "ext-workspace.h"
#include "foreign-toplevel.h"
#include "tablet.h"
#include "tearing.h"
#include "text-input.h"
