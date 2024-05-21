#include "Core.h"
#if defined CC_BUILD_TERMINAL
#include "_WindowBase.h"
#include "String.h"
#include "Funcs.h"
#include "Bitmap.h"
#include "Options.h"
#include "Errors.h"
#include "Utils.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#ifdef CC_BUILD_LINUX
#include <sys/kd.h>
#include <linux/keyboard.h>
#endif

// Inspired from https://github.com/Cubified/tuibox/blob/main/tuibox.h#L606
// Uses '▄' to double the vertical resolution
// (this trick was inspired from https://github.com/ichinaski/pxl/blob/master/main.go#L30)
static struct termios tio;
static struct winsize ws;

#define CSI "\x1B["

#define ERASE_CMD(cmd)    CSI cmd "J"
#define DEC_PM_SET(cmd)   CSI "?" cmd "h"
#define DEC_PM_RESET(cmd) CSI "?" cmd "1"

#ifdef CC_BUILD_LINUX
static int orig_KB = K_XLATE;
#endif

static void UpdateDimensions(void) {
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);

	DisplayInfo.Width  = ws.ws_col;
	DisplayInfo.Height = ws.ws_row * 2;
	DisplayInfo.Depth  = 4;
	DisplayInfo.ScaleX = 0.5f;
	DisplayInfo.ScaleY = 0.5f;

	Window_Main.Width  = DisplayInfo.Width;
	Window_Main.Height = DisplayInfo.Height;	
}

static void HookTerminal(void) {
	struct termios raw;
	
	tcgetattr(STDIN_FILENO, &tio);
	raw = tio;
	raw.c_lflag &= ~(ECHO | ICANON);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
	
	// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h3-Normal-tracking-mode
	printf(DEC_PM_SET("1049")); // Use Normal Screen Buffer and restore cursor as in DECRC, xterm.
	printf(CSI "0m");
	printf(ERASE_CMD("2")); // Ps = 2  ⇒  Erase All.
	printf(DEC_PM_SET("1003")); // Ps = 1 0 0 3  ⇒  Use All Motion Mouse Tracking, xterm.  See
	printf(DEC_PM_SET("1015")); // Ps = 1 0 1 5  ⇒  Enable urxvt Mouse Mode.
	printf(DEC_PM_SET("1006")); // Ps = 1 0 0 6  ⇒  Enable SGR Mouse Mode, xterm.
	printf(DEC_PM_RESET("25")); // Ps = 2 5  ⇒  Show cursor (DECTCEM), VT220.
}

static void UnhookTerminal(void) {
	//ioctl(STDIN_FILENO, KDSKBMODE, orig_KB);	
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &tio);
	
	printf(DEC_PM_RESET("1049"));
	printf(CSI "0m");
	printf(ERASE_CMD("2")); // Ps = 2  ⇒  Erase All.
	printf(DEC_PM_RESET("1003"));
	printf(DEC_PM_RESET("1015"));
	printf(DEC_PM_RESET("1006"));
	printf(DEC_PM_SET("25"));
}

static cc_bool pendingResize, pendingClose;
static void sigwinch_handler(int sig) { pendingResize = true; }
static void sigterm_handler(int sig)  { pendingClose  = true; UnhookTerminal(); }

void Window_Init(void) {
	Input.Sources = INPUT_SOURCE_NORMAL;

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
	//ioctl(STDIN_FILENO , KDGKBMODE, &orig_KB);
	//ioctl(STDIN_FILENO,  KDSKBMODE, K_MEDIUMRAW);
	HookTerminal();

	UpdateDimensions();
	signal(SIGWINCH, sigwinch_handler);
	signal(SIGTERM,  sigterm_handler);
	signal(SIGINT,   sigterm_handler);
}

void Window_Free(void) {
	UnhookTerminal();
}

static void DoCreateWindow(int width, int height) {
	Window_Main.Exists = true;
	Window_Main.Handle = (void*)1;
	Window_Main.Focused = true;
}
void Window_Create2D(int width, int height) { DoCreateWindow(width, height); }
void Window_Create3D(int width, int height) { DoCreateWindow(width, height); }

void Window_SetTitle(const cc_string* title) {
	// TODO
}

void Clipboard_GetText(cc_string* value) {
	// TODO
}

void Clipboard_SetText(const cc_string* value) {
	// TODO
}

int Window_GetWindowState(void) {
	return WINDOW_STATE_NORMAL;
}

cc_result Window_EnterFullscreen(void) {
	return 0;
}
cc_result Window_ExitFullscreen(void) {
	return 0;
}

int Window_IsObscured(void) { return 0; }

void Window_Show(void) { }

void Window_SetSize(int width, int height) {
	// TODO
}

void Window_RequestClose(void) {
	// TODO
}

static int MapNativeMouse(int button) {
	if (button == 1) return CCMOUSE_L;
	if (button == 2) return CCMOUSE_M;
	if (button == 3) return CCMOUSE_R;

	if (button ==  8) return CCMOUSE_X1;
	if (button ==  9) return CCMOUSE_X2;
	if (button == 10) return CCMOUSE_X3;
	if (button == 11) return CCMOUSE_X4;
	if (button == 12) return CCMOUSE_X5;
	if (button == 13) return CCMOUSE_X6;

	/* Mouse horizontal and vertical scroll */
	if (button >= 4 && button <= 7) return 0;
	Platform_Log1("Unknown mouse button: %i", &button);
	return 0;
}

static int stdin_available(void) {
    struct pollfd pfd;
    pfd.fd     = STDIN_FILENO;
    pfd.events = POLLIN;

    if (poll(&pfd, 1, 0)) {
		if (pfd.revents & POLLIN) return 1;
    }
    return 0;
}

#define ExtractXY() \
  tok = strtok(NULL, ";"); \
  x   = atoi(tok); \
  tok = strtok(NULL, ";"); \
  y   = atoi(tok) * 2;

static void ProcessMouse(char* buf, int n) {
	char cpy[256+2];
	strncpy(cpy, buf, n);
	char* tok = strtok(cpy + 3, ";");
	int x, y, mouse;
	if (!tok) return;

    switch (tok[0]){
	case '0':
		mouse = strchr(buf, 'm') == NULL;
		ExtractXY();
		Pointer_SetPosition(0, x, y);
		Input_SetNonRepeatable(CCMOUSE_L, mouse);
		break;
	case '3':
		mouse = (strcmp(tok, "32") == 0);
		ExtractXY();
		Pointer_SetPosition(0, x, y);
	break;
    }
}

static void ProcessKey(int key) {
	if (key >= 'a' && key <= 'z') key -= 32;
	
	if (key >= 'A' && key <= 'Z') {
		Input_SetPressed(key);
//		Input_SetReleased(key);
	} else if (key == ' ') {
		Input_SetPressed(CCKEY_SPACE);
		Input_SetReleased(CCKEY_SPACE);
	}
}

void Window_ProcessEvents(float delta) {
	char buf[256];
	int n;
	
	if (pendingResize) {
		pendingResize = false;
		UpdateDimensions();
		Event_RaiseVoid(&WindowEvents.Resized);
	}
	
	if (pendingClose) {
		pendingClose = false;
		Window_Main.Exists = false;
		Event_RaiseVoid(&WindowEvents.Closing);
		return;
	}
	
	if (!stdin_available()) return;
	n = read(STDIN_FILENO, buf, sizeof(buf));
	int A = buf[0];
	//Platform_Log2("IN: %i, %i", &n, &A);

	if (n >= 4 && buf[0] == '\x1b' && buf[1] == '[' && buf[2] == '<') {
		ProcessMouse(buf, n);
	} else if (buf[0] >= 32 && buf[0] < 127) {
		ProcessKey(buf[0]);
	}
	// TODO
}

void Window_ProcessGamepads(float delta) { }

static void Cursor_GetRawPos(int* x, int* y) {
	*x = 0;
	*y = 0;
	// TODO
}

void Cursor_SetPosition(int x, int y) {
	// TODO
}

static void Cursor_DoSetVisible(cc_bool visible) {
	// TODO
}


static void ShowDialogCore(const char* title, const char* msg) {
	Platform_LogConst(title);
	Platform_LogConst(msg);
}

cc_result Window_OpenFileDialog(const struct OpenFileDialogArgs* args) {
	return ERR_NOT_SUPPORTED;
}

cc_result Window_SaveFileDialog(const struct SaveFileDialogArgs* args) {
	return ERR_NOT_SUPPORTED;
}


void Window_AllocFramebuffer(struct Bitmap* bmp) {
	bmp->scan0 = (BitmapCol*)Mem_Alloc(bmp->width * bmp->height, 4, "window pixels");
}

void Window_DrawFramebuffer(Rect2D r, struct Bitmap* bmp) {
	for (int y = r.y & ~0x01; y < r.y + r.height; y += 2)
		for (int x = r.x; x < r.x + r.width; x++)
	{
		BitmapCol top = Bitmap_GetPixel(bmp, x, y + 0);
		BitmapCol bot = Bitmap_GetPixel(bmp, x, y + 1);

		// Use '▄' so each cell can use a background and foreground colour
		// This essentially doubles the vertical resolution of the displayed image
		printf(CSI "48;2;%i;%i;%im", BitmapCol_R(top), BitmapCol_G(top), BitmapCol_B(top));
		printf(CSI "38;2;%i;%i;%im", BitmapCol_R(bot), BitmapCol_G(bot), BitmapCol_B(bot));
		printf(CSI "%i;%iH\xE2\x96\x84", y / 2, x);
	}
}

void Window_FreeFramebuffer(struct Bitmap* bmp) {
	Mem_Free(bmp->scan0);
}

void OnscreenKeyboard_Open(struct OpenKeyboardArgs* args) { }
void OnscreenKeyboard_SetText(const cc_string* text) { }
void OnscreenKeyboard_Draw2D(Rect2D* r, struct Bitmap* bmp) { }
void OnscreenKeyboard_Draw3D(void) { }
void OnscreenKeyboard_Close(void) { }

void Window_EnableRawMouse(void) {
	DefaultEnableRawMouse();
}

void Window_UpdateRawMouse(void) {
	CentreMousePosition();
}

void Window_DisableRawMouse(void) {
	DefaultDisableRawMouse();
}
#endif
