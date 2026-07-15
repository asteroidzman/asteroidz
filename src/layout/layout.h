static void overview(Monitor *m);
static void scroller(Monitor *m);
static void monocle(Monitor *m);
static void dwindle(Monitor *m);
static void floatlayout(Monitor *m);

/* layout(s) */
Layout overviewlayout = {"󰃇", overview, "overview"};

enum {
	DWINDLE,
	SCROLLER,
	MONOCLE,
	FLOATING,
};

Layout layouts[] = {
	// at least two entries; don't shrink this below two
	// layouts[0] is the default for freshly-created tags/monitors
	/* symbol     arrange function   name */
	{"T", dwindle, "tile", DWINDLE},		 // manual-control tiled layout (i3-like)
	{"S", scroller, "scroller", SCROLLER},	 // scroller layout
	{"M", monocle, "monocle", MONOCLE},	 // monocle layout
	{"F", floatlayout, "float", FLOATING},	 // floating layout (cascading)
};