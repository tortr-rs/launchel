/* launchel - one line to type what you want to run, like window maker's */
#include <ctype.h>
#include <dirent.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#define LENGTH(X)       (sizeof(X) / sizeof(X)[0])
#define MIN(A, B)       ((A) < (B) ? (A) : (B))
#define MAX(A, B)       ((A) > (B) ? (A) : (B))
#define MAXITEMS        8192

typedef struct {
	char *name;	/* what you see and type */
	char *cmd;	/* what runs */
	int used;	/* how often, from the history */
	int app;	/* from a .desktop file */
} Item;

/* settings, from ~/.config/launchel/config */
static char fontname[256] = "VGA", fgname[32] = "grey", bgname[32] = "black", selname[32] = "grey";
static char terminal[256] = "ttywm-term", place[16] = "bottom";
static int fontsize;

static const char defaultconfig[] =
"# launchel settings\n"
"\n"
"font = VGA\n"
"# nothing means ttywm's size, or 16\n"
"font size =\n"
"# top or bottom of the screen\n"
"place = bottom\n"
"# what runs programs that want a terminal, and anything you start with !\n"
"terminal = ttywm-term\n"
"\n"
"# colours: black blue green cyan red magenta brown grey\n"
"#          dark grey, light blue, light green, light cyan, light red, pink, yellow, white\n"
"#          or #rrggbb\n"
"text colour = grey\n"
"background colour = black\n"
"highlight colour = grey\n"
"\n"
"# type and press enter. tab and the arrows pick another match,\n"
"# shift+enter runs exactly what you typed, !htop runs htop in a terminal, escape goes away\n";

static Display *dpy;
static Window win, root;
static int scr, sw, sh, cw, ch, ascent;
static XftFont *font;
static XftDraw *draw;
static XftColor fg, bg, hl;
static Pixmap buf;
static GC gc;
static XIC xic;

static Item items[MAXITEMS];
static int nitems;
static Item *match[MAXITEMS];
static int nmatch, cur, first;	/* first: the leftmost match shown */
static char text[1024];
static size_t len;
static char histpath[1100];

/* ---- small things ---- */

static void
die(const char *msg)
{
	fprintf(stderr, "launchel: %s\n", msg);
	exit(1);
}

static char *
trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	for (e = s + strlen(s); e > s && isspace((unsigned char)e[-1]); e--);
	*e = '\0';
	return s;
}

static void
squash(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	for (; *src && i < n - 1; src++)
		if (!isspace((unsigned char)*src))
			dst[i++] = tolower((unsigned char)*src);
	dst[i] = '\0';
}

static const char *
colourhex(const char *name)
{
	static const char *vga[][2] = {
		{ "black", "#000000" }, { "blue", "#0000aa" }, { "green", "#00aa00" }, { "cyan", "#00aaaa" },
		{ "red", "#aa0000" }, { "magenta", "#aa00aa" }, { "brown", "#aa5500" }, { "grey", "#aaaaaa" },
		{ "gray", "#aaaaaa" }, { "darkgrey", "#555555" }, { "darkgray", "#555555" },
		{ "lightblue", "#5555ff" }, { "lightgreen", "#55ff55" }, { "lightcyan", "#55ffff" },
		{ "lightred", "#ff5555" }, { "pink", "#ff55ff" }, { "yellow", "#ffff55" }, { "white", "#ffffff" },
	};
	char s[32];
	unsigned int i;

	squash(s, name, sizeof s);
	for (i = 0; i < LENGTH(vga); i++)
		if (!strcmp(s, vga[i][0]))
			return vga[i][1];
	return name[0] == '#' ? name : "#aaaaaa";
}

static void
colour(const char *name, XftColor *c)
{
	if (!XftColorAllocName(dpy, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr), colourhex(name), c))
		XftColorAllocName(dpy, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr), "#aaaaaa", c);
}

static int
textw(const char *s)
{
	XGlyphInfo gi;

	XftTextExtentsUtf8(dpy, font, (const FcChar8 *)s, strlen(s), &gi);
	return gi.xOff;
}

/* ---- settings ---- */

static void
loadconfig(const char *path)
{
	char line[1024], key[64], *name, *value, *eq;
	struct stat st;
	FILE *f;

	if (stat(path, &st) < 0 && (f = fopen(path, "w"))) {
		fputs(defaultconfig, f);
		fclose(f);
	}
	if (!(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof line, f)) {
		if (*trim(line) == '#' || !(eq = strchr(line, '=')))
			continue;
		*eq = '\0';
		name = trim(line);
		value = trim(eq + 1);
		squash(key, name, sizeof key);
		if (!strcmp(key, "font"))
			snprintf(fontname, sizeof fontname, "%s", *value ? value : "VGA");
		else if (!strcmp(key, "fontsize"))
			fontsize = atoi(value);
		else if (!strcmp(key, "place"))
			squash(place, value, sizeof place);
		else if (!strcmp(key, "terminal"))
			snprintf(terminal, sizeof terminal, "%s", *value ? value : "ttywm-term");
		else if (!strcmp(key, "textcolour") || !strcmp(key, "textcolor"))
			snprintf(fgname, sizeof fgname, "%s", value);
		else if (!strcmp(key, "backgroundcolour") || !strcmp(key, "backgroundcolor"))
			snprintf(bgname, sizeof bgname, "%s", value);
		else if (!strcmp(key, "highlightcolour") || !strcmp(key, "highlightcolor"))
			snprintf(selname, sizeof selname, "%s", value);
		else
			fprintf(stderr, "launchel: %s: don't know '%s'\n", path, name);
	}
	fclose(f);
}

/* ---- what there is to run ---- */

static Item *
find(const char *name)
{
	int i;

	for (i = 0; i < nitems; i++)
		if (!strcmp(items[i].name, name))
			return &items[i];
	return NULL;
}

static void
add(const char *name, const char *cmd, int app)
{
	Item *it;

	if (nitems == MAXITEMS || !*name || ((it = find(name)) && (it->app || !app)))
		return;
	if (it) {
		/* an app by the same name as a program: the app wins, it knows its arguments */
		free(it->cmd);
		it->cmd = strdup(cmd);
		it->app = app;
		return;
	}
	items[nitems].name = strdup(name);
	items[nitems].cmd = strdup(cmd);
	items[nitems].app = app;
	items[nitems++].used = 0;
}

/* Exec= without the %f and %u bits */
static void
cleanexec(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	for (; *src && i < n - 1; src++) {
		if (*src == '%' && src[1]) {
			src++;
			if (*src == '%')
				dst[i++] = '%';
			continue;
		}
		dst[i++] = *src;
	}
	while (i > 0 && dst[i - 1] == ' ')
		i--;
	dst[i] = '\0';
}

static void
readdesktop(const char *path)
{
	char line[2048], name[256] = "", exec[1024] = "", cmd[1400], *v;
	int inentry = 0, hidden = 0, term = 0;
	FILE *f;

	if (!(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof line, f)) {
		trim(line);
		if (line[0] == '[') {
			inentry = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!inentry || !(v = strchr(line, '=')))
			continue;
		*v++ = '\0';
		if (!strcmp(line, "Name") && !*name)
			snprintf(name, sizeof name, "%s", v);
		else if (!strcmp(line, "Exec"))
			cleanexec(exec, v, sizeof exec);
		else if ((!strcmp(line, "NoDisplay") || !strcmp(line, "Hidden")) && !strcmp(v, "true"))
			hidden = 1;
		else if (!strcmp(line, "Terminal") && !strcmp(v, "true"))
			term = 1;
		else if (!strcmp(line, "Type") && strcmp(v, "Application"))
			hidden = 1;
	}
	fclose(f);
	if (hidden || !*name || !*exec)
		return;
	if (term)
		snprintf(cmd, sizeof cmd, "%s -e %s", terminal, exec);
	else
		snprintf(cmd, sizeof cmd, "%s", exec);
	add(name, cmd, 1);
}

static void
readapps(const char *dir)
{
	char path[2048];
	struct dirent *d;
	DIR *dp;
	size_t n;

	if (!(dp = opendir(dir)))
		return;
	while ((d = readdir(dp))) {
		n = strlen(d->d_name);
		if (n > 8 && !strcmp(d->d_name + n - 8, ".desktop")) {
			snprintf(path, sizeof path, "%s/%s", dir, d->d_name);
			readdesktop(path);
		}
	}
	closedir(dp);
}

static void
readpath(void)
{
	char *path = getenv("PATH"), *p, *dir, file[4096];
	struct dirent *d;
	struct stat st;
	DIR *dp;

	if (!path)
		return;
	p = path = strdup(path);
	while ((dir = strsep(&p, ":"))) {
		if (!*dir || !(dp = opendir(dir)))
			continue;
		while ((d = readdir(dp))) {
			if (d->d_name[0] == '.')
				continue;
			snprintf(file, sizeof file, "%s/%s", dir, d->d_name);
			if (stat(file, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111))
				add(d->d_name, d->d_name, 0);
		}
		closedir(dp);
	}
	free(path);
}

/* the history: one name a line, the newest at the bottom */
static void
readhistory(void)
{
	char line[1100];
	Item *it;
	FILE *f;

	if (!(f = fopen(histpath, "r")))
		return;
	while (fgets(line, sizeof line, f)) {
		trim(line);
		if ((it = find(line)))
			it->used++;
		else if (*line)
			add(line, line, 0), find(line)->used = 1;
	}
	fclose(f);
}

static void
remember(const char *name)
{
	char *lines[500], line[1100];
	int n = 0, i;
	FILE *f;

	/* keep the last 500 */
	if ((f = fopen(histpath, "r"))) {
		while (n < (int)LENGTH(lines) && fgets(line, sizeof line, f))
			lines[n++] = strdup(line);
		fclose(f);
	}
	if (!(f = fopen(histpath, "w")))
		return;
	for (i = n >= (int)LENGTH(lines) ? 1 : 0; i < n; i++)
		fputs(lines[i], f);
	fprintf(f, "%s\n", name);
	fclose(f);
	for (i = 0; i < n; i++)
		free(lines[i]);
}

/* ---- matching ---- */

static int
score(Item *it, const char *q)
{
	size_t n = strlen(q);

	if (!n)
		return it->used ? 1000 + it->used : -1;
	if (!strncasecmp(it->name, q, n))
		return 3000 + it->used * 10 + (it->app ? 5 : 0) - (int)strlen(it->name);
	/* the start of a word in it: "fox" finds "Firefox" less than "fir" does */
	if (strcasestr(it->name, q))
		return 2000 + it->used * 10 + (it->app ? 5 : 0) - (int)strlen(it->name);
	return -1;
}

static int scores[MAXITEMS];

static int
bysc(const void *a, const void *b)
{
	const Item *x = *(Item *const *)a, *y = *(Item *const *)b;
	int d = scores[y - items] - scores[x - items];

	return d ? d : strcasecmp(x->name, y->name);
}

static void
domatch(void)
{
	char q[1024], *sp;
	int i;

	/* only the first word is matched: "firefox github.com" still finds firefox */
	snprintf(q, sizeof q, "%s", text[0] == '!' ? text + 1 : text);
	if ((sp = strchr(q, ' ')))
		*sp = '\0';
	nmatch = 0;
	for (i = 0; i < nitems; i++)
		if ((scores[i] = score(&items[i], q)) >= 0)
			match[nmatch++] = &items[i];
	qsort(match, nmatch, sizeof *match, bysc);
	cur = first = 0;
}

/* ---- running it ---- */

static void
spawn(const char *cmd)
{
	pid_t pid;

	if ((pid = fork()) == 0) {
		/* twice, so it isn't ours when we're gone */
		if (fork() == 0) {
			setsid();
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

static void
go(int exact)
{
	char cmd[2400], *args;
	const char *t = text[0] == '!' ? text + 1 : text;
	Item *it = !exact && nmatch ? match[cur] : NULL;

	while (*t == ' ')
		t++;
	if (!*t && !it)
		return;
	args = strchr(t, ' ');
	if (it && args)
		/* the match, and what you typed after its name */
		snprintf(cmd, sizeof cmd, "%s%s", it->cmd, args);
	else if (it)
		snprintf(cmd, sizeof cmd, "%s", it->cmd);
	else
		snprintf(cmd, sizeof cmd, "%s", t);
	if (text[0] == '!') {
		char c2[2600];

		if (strchr(cmd, '\''))
			snprintf(c2, sizeof c2, "%s -hold -e %s", terminal, cmd);
		else
			snprintf(c2, sizeof c2, "%s -hold -e sh -c '%s'", terminal, cmd);
		snprintf(cmd, sizeof cmd, "%s", c2);
	}
	remember(it ? it->name : t);
	spawn(cmd);
}

/* ---- drawing ---- */

static void
drawbar(void)
{
	char s[1100];
	int x, w, i, avail;

	XftDrawRect(draw, &bg, 0, 0, sw, ch);
	snprintf(s, sizeof s, "run: %s", text);
	XftDrawStringUtf8(draw, &fg, font, 0, ascent, (FcChar8 *)s, strlen(s));
	x = textw(s);
	XftDrawRect(draw, &fg, x, 0, cw, ch);	/* the cursor */
	x += cw * 3;
	/* keep the one picked in sight */
	avail = sw - x;
	if (cur < first)
		first = cur;
	for (;;) {
		w = 0;
		for (i = first; i <= cur && i < nmatch; i++)
			w += textw(match[i]->name) + cw * 2;
		if (w <= avail || first >= cur)
			break;
		first++;
	}
	if (first > 0) {
		XftDrawStringUtf8(draw, &fg, font, x, ascent, (FcChar8 *)"<", 1);
		x += cw * 2;
	}
	for (i = first; i < nmatch; i++) {
		w = textw(match[i]->name);
		if (x + w + cw * 2 > sw) {
			XftDrawStringUtf8(draw, &fg, font, sw - cw, ascent, (FcChar8 *)">", 1);
			break;
		}
		if (i == cur) {
			XftDrawRect(draw, &hl, x - cw / 2, 0, w + cw, ch);
			XftDrawStringUtf8(draw, &bg, font, x, ascent, (FcChar8 *)match[i]->name, strlen(match[i]->name));
		} else
			XftDrawStringUtf8(draw, &fg, font, x, ascent, (FcChar8 *)match[i]->name, strlen(match[i]->name));
		x += w + cw * 2;
	}
	XCopyArea(dpy, buf, win, gc, 0, 0, sw, ch, 0, 0);
}

/* ---- keys ---- */

static int
keypress(XKeyEvent *e)
{
	char s[64];
	KeySym sym;
	Status st;
	int n = xic ? Xutf8LookupString(xic, e, s, sizeof s - 1, &sym, &st) : XLookupString(e, s, sizeof s - 1, &sym, NULL);
	int ctrl = e->state & ControlMask, shift = e->state & ShiftMask;

	s[MAX(n, 0)] = '\0';
	if (ctrl) {
		switch (sym) {
		case XK_u: len = 0; text[0] = '\0'; domatch(); return 0;
		case XK_w:
			while (len > 0 && text[len - 1] == ' ')
				len--;
			while (len > 0 && text[len - 1] != ' ')
				len--;
			text[len] = '\0';
			domatch();
			return 0;
		case XK_n: sym = XK_Right; break;
		case XK_p: sym = XK_Left; break;
		case XK_c: case XK_g: case XK_bracketleft: return 1;
		case XK_Return: go(1); return 1;
		default: return 0;
		}
	}
	switch (sym) {
	case XK_Escape:
		return 1;
	case XK_Return: case XK_KP_Enter:
		go(shift);
		return 1;
	case XK_Tab: case XK_Right: case XK_Down:
		if (nmatch)
			cur = (cur + 1) % nmatch;
		return 0;
	case XK_ISO_Left_Tab: case XK_Left: case XK_Up:
		if (nmatch)
			cur = (cur + nmatch - 1) % nmatch;
		return 0;
	case XK_BackSpace:
		/* a whole letter, not a byte of it */
		while (len > 0 && (text[--len] & 0xc0) == 0x80);
		text[len] = '\0';
		domatch();
		return 0;
	}
	if (n > 0 && (unsigned char)s[0] >= 0x20 && s[0] != 0x7f && len + n < sizeof text) {
		memcpy(text + len, s, n);
		len += n;
		text[len] = '\0';
		domatch();
	}
	return 0;
}

static void
grab(void)
{
	struct timespec ts = { 0, 1000000 };
	int i;

	/* the window manager might still have the keyboard for the key that started us */
	for (i = 0; i < 1000; i++) {
		if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
			return;
		nanosleep(&ts, NULL);
	}
	die("can't have the keyboard");
}

int
main(int argc, char *argv[])
{
	XSetWindowAttributes wa;
	XClassHint hint = { "launchel", "launchel" };
	XIM xim;
	XEvent ev;
	char dir[1100], path[1200];
	const char *home = getenv("HOME") ? getenv("HOME") : ".", *xdg, *size;
	int y;

	if (argc == 2 && !strcmp(argv[1], "-v")) {
		puts("launchel-" VERSION);
		return 0;
	} else if (argc != 1)
		die("usage: launchel [-v]");
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");
	if (!(dpy = XOpenDisplay(NULL)))
		die("can't open the display");
	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	sw = DisplayWidth(dpy, scr);
	sh = DisplayHeight(dpy, scr);

	if ((xdg = getenv("XDG_CONFIG_HOME")) && *xdg)
		snprintf(dir, sizeof dir, "%s/launchel", xdg);
	else
		snprintf(dir, sizeof dir, "%s/.config/launchel", home);
	snprintf(path, sizeof path, "%s/.config", home);
	mkdir(path, 0755);
	mkdir(dir, 0755);
	snprintf(path, sizeof path, "%s/config", dir);
	loadconfig(path);
	snprintf(histpath, sizeof histpath, "%s/history", dir);

	if (!fontsize)
		fontsize = (size = getenv("TTYWM_SIZE")) && atoi(size) > 3 ? atoi(size) : 16;
	snprintf(path, sizeof path, "%s:pixelsize=%d", fontname, fontsize);
	if (!(font = XftFontOpenName(dpy, scr, path)) && !(font = XftFontOpenName(dpy, scr, "monospace:pixelsize=16")))
		die("no font");
	cw = textw("M");
	ch = font->ascent + font->descent;
	ascent = font->ascent;
	colour(fgname, &fg);
	colour(bgname, &bg);
	colour(selname, &hl);

	/* show the bar before reading every program there is */
	y = !strcmp(place, "top") ? 0 : sh - ch;
	wa.override_redirect = True;
	wa.background_pixel = BlackPixel(dpy, scr);
	wa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask;
	win = XCreateWindow(dpy, root, 0, y, sw, ch, 0, CopyFromParent, CopyFromParent, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
	XSetClassHint(dpy, win, &hint);
	XStoreName(dpy, win, "launchel");
	buf = XCreatePixmap(dpy, win, sw, ch, DefaultDepth(dpy, scr));
	draw = XftDrawCreate(dpy, buf, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr));
	gc = XCreateGC(dpy, win, 0, NULL);
	if ((xim = XOpenIM(dpy, NULL, NULL, NULL)))
		xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win,
		                XNFocusWindow, win, NULL);
	XMapRaised(dpy, win);
	grab();
	drawbar();
	XFlush(dpy);

	snprintf(path, sizeof path, "%s/.local/share/applications", home);
	readapps(path);
	readapps("/usr/local/share/applications");
	readapps("/usr/share/applications");
	readapps("/var/lib/flatpak/exports/share/applications");
	snprintf(path, sizeof path, "%s/.local/share/flatpak/exports/share/applications", home);
	readapps(path);
	readpath();
	readhistory();
	domatch();
	drawbar();

	while (!XNextEvent(dpy, &ev)) {
		if (XFilterEvent(&ev, None))
			continue;
		if (ev.type == Expose)
			drawbar();
		else if (ev.type == KeyPress) {
			if (keypress(&ev.xkey))
				break;
			drawbar();
		} else if (ev.type == ButtonPress)
			break;	/* a click anywhere: never mind */
	}
	XUngrabKeyboard(dpy, CurrentTime);
	XCloseDisplay(dpy);
	return 0;
}
