#include "Core.h"
#if defined CC_BUILD_HAIKU && !defined CC_BUILD_SDL
extern "C" {
#include "_WindowBase.h"
#include "Graphics.h"
#include "String.h"
#include "Funcs.h"
#include "Bitmap.h"
#include "Errors.h"
#include "Utils.h"
}

#include <AppKit.h>
#include <InterfaceKit.h>
#include <OpenGLKit.h>
#include <StorageKit.h>

static BApplication* app_handle;
static BWindow* win_handle;
static BView* view_handle;
static BGLView* view_3D;

// Event management
enum CCEventType {
	CC_NONE,
	CC_MOUSE_SCROLL, CC_MOUSE_DOWN, CC_MOUSE_UP, CC_MOUSE_MOVE,
	CC_KEY_DOWN, CC_KEY_UP, CC_KEY_INPUT,
	CC_WIN_RESIZED, CC_WIN_FOCUS, CC_WIN_REDRAW, CC_WIN_QUIT
};
union CCEventValue { float f32; int i32; void* ptr; };
struct CCEvent {
	int type;
	CCEventValue v1, v2;
};

#define EVENTS_DEFAULT_MAX 20
static void* events_mutex;
static int events_count, events_capacity;
static CCEvent* events_list, events_default[EVENTS_DEFAULT_MAX];

static void Events_Init(void) {
	events_mutex    = Mutex_Create();
	events_capacity = EVENTS_DEFAULT_MAX;
	events_list     = events_default;
}

static void Events_Push(const CCEvent* event) {
	Mutex_Lock(events_mutex);
	{
		if (events_count >= events_capacity) {
			Utils_Resize((void**)&events_list, &events_capacity,
						sizeof(CCEvent), EVENTS_DEFAULT_MAX, 20);
		}
		events_list[events_count++] = *event;
	}
	Mutex_Unlock(events_mutex);
}

static cc_bool Events_Pull(CCEvent* event) {
	cc_bool found = false;
	
	Mutex_Lock(events_mutex);
	{
		if (events_count) {
			*event = events_list[0];
			for (int i = 1; i < events_count; i++) {
				events_list[i - 1] = events_list[i];
			}
			events_count--;
			found = true;
		}
	}
	Mutex_Unlock(events_mutex);
	return found;
}

// BApplication implementation
class CC_BApp : public BApplication
{
public:
	CC_BApp() : BApplication("application/x-ClassiCube") { }
	void DispatchMessage(BMessage* msg, BHandler* handler);
};

static void CallOpenFileCallback(const char* path);
void CC_BApp::DispatchMessage(BMessage* msg, BHandler* handler) {
	CCEvent event = { 0 };
	entry_ref fileRef;
	
	switch (msg->what)
	{
	case B_QUIT_REQUESTED:
		Platform_LogConst("APP QUIT");
		event.type = CC_WIN_QUIT;
		break;
	case B_REFS_RECEIVED:
		// TODO do we need to support more than 1 ref?
		if (msg->FindRef("refs", 0, &fileRef) == B_OK) {
			BPath path(&fileRef);
			CallOpenFileCallback(path.Path());
		}
		break;
	default:
		//Platform_LogConst("UNHANDLED APP MESSAGE:");
		//msg->PrintToStream();
		break;
	}
	if (event.type) Events_Push(&event);
	BApplication::DispatchMessage(msg, handler);
}

// BWindow implementation
class CC_BWindow : public BWindow
{
	public:
		CC_BWindow(BRect frame) : BWindow(frame, "", B_TITLED_WINDOW, 0) { }
		void DispatchMessage(BMessage* msg, BHandler* handler);
};

static void ProcessKeyInput(BMessage* msg) {
	CCEvent event;
	const char* value;
	cc_codepoint cp;
	
	if (msg->FindString("bytes", &value) != B_OK) return;
	if (!Convert_Utf8ToCodepoint(&cp, (const cc_uint8*)value, String_Length(value))) return;
	
	event.type = CC_KEY_INPUT;
	event.v1.i32 = cp;
	Events_Push(&event);
}

static int last_buttons;
static void UpdateMouseButton(int btn, int pressed) {
	CCEvent event;
	event.type   = pressed ? CC_MOUSE_DOWN : CC_MOUSE_UP;
	event.v1.i32 = btn;
	Events_Push(&event);
}

static void UpdateMouseButtons(int buttons) {
	// BeOS API is really odd in that it only provides you with a bitmask
	//  of 'current mouse buttons pressed'
	int changed  = buttons ^ last_buttons;
	
	// TODO move logic to UpdateMouseButton instead?
	if (changed & B_PRIMARY_MOUSE_BUTTON)   
		UpdateMouseButton(KEY_LMOUSE, buttons & B_PRIMARY_MOUSE_BUTTON);
	if (changed & B_SECONDARY_MOUSE_BUTTON)
		UpdateMouseButton(KEY_RMOUSE, buttons & B_SECONDARY_MOUSE_BUTTON);
	if (changed & B_TERTIARY_MOUSE_BUTTON) 
		UpdateMouseButton(KEY_MMOUSE, buttons & B_TERTIARY_MOUSE_BUTTON);
	last_buttons = buttons;
}

void CC_BWindow::DispatchMessage(BMessage* msg, BHandler* handler) {
	CCEvent event = { 0 };
	BPoint where;
	float delta;
	int value, width, height;
	bool active;
	
	switch (msg->what)
	{
	case B_KEY_DOWN:
	case B_UNMAPPED_KEY_DOWN:
		if (msg->FindInt32("key", &value) == B_OK) {
			event.type   = CC_KEY_DOWN;
			event.v1.i32 = value;
		} break;
	case B_KEY_UP:
	case B_UNMAPPED_KEY_UP:
		if (msg->FindInt32("key", &value) == B_OK) {
			event.type   = CC_KEY_UP;
			event.v1.i32 = value;
		} break;
	case B_MOUSE_DOWN:
	case B_MOUSE_UP:
		if (msg->FindInt32("buttons", &value) == B_OK) {
			UpdateMouseButtons(value);
		} break;
	case B_MOUSE_MOVED:
		if (msg->FindPoint("where", &where) == B_OK) {
			event.type   = CC_MOUSE_MOVE;
			event.v1.i32 = where.x;
			event.v2.i32 = where.y;
		} break;
	case B_MOUSE_WHEEL_CHANGED:
		if (msg->FindFloat("be:wheel_delta_y", &delta) == B_OK) {
			event.type   = CC_MOUSE_SCROLL;
			event.v1.f32 = -delta; // negate to match other platforms
		} break;
		
	case B_WINDOW_ACTIVATED:
		if (msg->FindBool("active", &active) == B_OK) {
			event.type   = CC_WIN_FOCUS;
			event.v1.i32 = active;
		} break;
	case B_WINDOW_MOVED:
		break; // avoid unhandled message spam
	case B_WINDOW_RESIZED:
		if (msg->FindInt32("width",  &width)  == B_OK &&
			msg->FindInt32("height", &height) == B_OK) {
			event.type   = CC_WIN_RESIZED;
			// width/height is 1 less than actual width/height
			event.v1.i32 = width  + 1;
			event.v2.i32 = height + 1;
		} break;
	case B_QUIT_REQUESTED:
		event.type = CC_WIN_QUIT;
		Platform_LogConst("WINQUIT");
		break;
	case _UPDATE_:
		event.type = CC_WIN_REDRAW;
		break;
	default:
		//Platform_LogConst("UNHANDLED WIN MESSAGE:");
		//msg->PrintToStream();
		break;
	}
	
	if (event.type) Events_Push(&event);
	if (msg->what == B_KEY_DOWN) ProcessKeyInput(msg);
	BWindow::DispatchMessage(msg, handler);
}


static void AppThread(void) {
	app_handle = new CC_BApp();
	// runs forever
	app_handle->Run();
	// because there are multiple other threads relying
	//  on BApp connection, trying to delete the reference
	//  tends to break them and crash the game at exit
	//delete app_handle;
}

static void RunApp(void) {
	void* thread = Thread_Create(AppThread);
	Thread_Start2(thread, AppThread);
	Thread_Detach(thread);
	
	// wait for BApplication to be started in other thread
	do {
		Thread_Sleep(10);
	} while (!app_handle || app_handle->IsLaunching());
	
	Platform_LogConst("App initialised");
}

void Window_Init(void) {
	Events_Init();
	RunApp();
	BScreen screen(B_MAIN_SCREEN_ID);
	BRect frame = screen.Frame();
	
	// e.g. frame = (l:0.0, t:0.0, r:1023.0, b:767.0)
	//  so have to add 1 here for actual width/height
	DisplayInfo.Width  = frame.IntegerWidth()  + 1;
	DisplayInfo.Height = frame.IntegerHeight() + 1;
	DisplayInfo.ScaleX = 1;
	DisplayInfo.ScaleY = 1;
}

static void DoCreateWindow(int width, int height) {
	// https://www.haiku-os.org/docs/api/classBRect.html#details
	// right/bottom coordinates are inclusive of the coordinates,
	//  so need to subtract 1 to end up with correct width/height
	int x = Display_CentreX(width), y = Display_CentreY(height);
	BRect frame(x, y, x + width - 1, y + height - 1);
	win_handle = new CC_BWindow(frame);
	
	WindowInfo.Exists = true;
	WindowInfo.Handle = win_handle;
	
	frame = win_handle->Bounds();
	WindowInfo.Width  = frame.IntegerWidth()  + 1;
	WindowInfo.Height = frame.IntegerHeight() + 1;
}

void Window_Create2D(int width, int height) {
	DoCreateWindow(width, height);
	view_handle = new BView(win_handle->Bounds(), "CC_LAUNCHER",
						B_FOLLOW_ALL, 0);
	win_handle->AddChild(view_handle);
}

void Window_Create3D(int width, int height) {
	DoCreateWindow(width, height);
	view_3D = new BGLView(win_handle->Bounds(), "CC_GAME",
						B_FOLLOW_ALL, B_FRAME_EVENTS,
						BGL_RGB | BGL_ALPHA | BGL_DOUBLE | BGL_DEPTH);
	view_handle = view_3D;
	win_handle->AddChild(view_handle);
}

void Window_SetTitle(const cc_string* title) {
	char raw[NATIVE_STR_LEN];
	Platform_EncodeUtf8(raw, title);
	
	win_handle->Lock(); // TODO even need to lock/unlock?
	win_handle->SetTitle(raw);
	win_handle->Unlock();
}

void Clipboard_GetText(cc_string* value) {
	if (!be_clipboard->Lock()) return;
	
	BMessage* clip  = be_clipboard->Data();
	char* str       = NULL;
	ssize_t str_len = 0;
	
	clip->FindData("text/plain", B_MIME_TYPE, (const void**)&str, &str_len);
	if (str) String_AppendUtf8(value, str, str_len);
		
	be_clipboard->Unlock();
}

void Clipboard_SetText(const cc_string* value) {
	char str[NATIVE_STR_LEN];
	int str_len = Platform_EncodeUtf8(str, value);
	
	if (!be_clipboard->Lock()) return;
	be_clipboard->Clear();
	
	BMessage* clip = be_clipboard->Data();
	clip->AddData("text/plain", B_MIME_TYPE, str, str_len);
	be_clipboard->Commit();
	
	be_clipboard->Unlock();
}

static BRect win_rect;
static cc_bool win_fullscreen;

int Window_GetWindowState(void) {
	return win_fullscreen ? WINDOW_STATE_FULLSCREEN : WINDOW_STATE_NORMAL;
}

cc_result Window_EnterFullscreen(void) {
	// TODO is there a better fullscreen API to use
	win_fullscreen = true;
	win_rect = win_handle->Frame();
	
	BScreen screen(B_MAIN_SCREEN_ID);
	BRect screen_frame = screen.Frame();
	
	win_handle->Lock();
	win_handle->MoveTo(screen_frame.left, screen_frame.top);
	win_handle->ResizeTo(screen_frame.Width(), screen_frame.Height());
	win_handle->SetFlags(win_handle->Flags() & ~(B_NOT_RESIZABLE | B_NOT_ZOOMABLE));
	//win_handle->SetLook(B_NO_BORDER_WINDOW_LOOK); // TODO unnecessary?
	win_handle->Unlock();
	return 0;
}
cc_result Window_ExitFullscreen(void) {
	win_fullscreen = false;
	
	win_handle->Lock();
	win_handle->MoveTo(win_rect.left, win_rect.top);
	win_handle->ResizeTo(win_rect.Width(), win_rect.Height());
	win_handle->SetFlags(win_handle->Flags() | (B_NOT_RESIZABLE | B_NOT_ZOOMABLE));
	//win_handle->SetLook(B_TITLED_WINDOW_LOOK);
	win_handle->Unlock();
	return 0; 
}

int Window_IsObscured(void) { return 0; }

void Window_Show(void) {
	win_handle->Lock(); // TODO even need to lock/unlock ?
	win_handle->Show();
	win_handle->Unlock();
}

void Window_SetSize(int width, int height) {
	// See reason for -1 in DoCreateWindow
	win_handle->Lock(); // TODO even need to lock/unlock ?
	win_handle->ResizeTo(width - 1, height - 1);
	win_handle->Unlock();
}

void Window_Close(void) {
	BMessage* msg = new BMessage(B_QUIT_REQUESTED);
	app_handle->PostMessage(msg);
}

static const cc_uint8 key_map[] = {
	/* 0x00 */ 0,KEY_ESCAPE,KEY_F1,KEY_F2, KEY_F3,KEY_F4,KEY_F5,KEY_F6, 
	/* 0x08 */ KEY_F7,KEY_F8,KEY_F9,KEY_F10, KEY_F11,KEY_F12,KEY_PRINTSCREEN,KEY_SCROLLLOCK,
	/* 0x10 */ KEY_PAUSE,KEY_TILDE,'1','2', '3','4','5','6',
	/* 0x18 */ '7','8','9','0', KEY_MINUS,KEY_EQUALS,KEY_BACKSPACE,KEY_INSERT,
	/* 0x20 */ KEY_HOME,KEY_PAGEUP,KEY_NUMLOCK,KEY_KP_DIVIDE, KEY_KP_MULTIPLY,KEY_KP_MINUS,KEY_TAB,'Q',
	/* 0x28 */ 'W','E','R','T', 'Y','U','I','O',
	/* 0x30 */ 'P',KEY_LBRACKET,KEY_RBRACKET,KEY_BACKSLASH, KEY_DELETE,KEY_END,KEY_PAGEDOWN,KEY_KP7,
	/* 0x38 */ KEY_KP8,KEY_KP9,KEY_KP_PLUS,KEY_CAPSLOCK, 'A','S','D','F',
	/* 0x40 */ 'G','H','J','K', 'L',KEY_SEMICOLON,KEY_QUOTE,KEY_ENTER,	
	/* 0x48 */ KEY_KP4,KEY_KP5,KEY_KP6,KEY_LSHIFT, 'Z','X','C','V',	
	/* 0x50 */ 'B','N','M',KEY_COMMA, KEY_PERIOD,KEY_SLASH,KEY_RSHIFT,KEY_UP,	
	/* 0x58 */ KEY_KP1,KEY_KP2,KEY_KP3,KEY_KP_ENTER, KEY_LCTRL,KEY_LALT,KEY_SPACE,KEY_RALT,	
	/* 0x60 */ KEY_RCTRL,KEY_LEFT,KEY_DOWN,KEY_RIGHT, KEY_KP0,KEY_KP_DECIMAL,KEY_LWIN,0,	
	/* 0x68 */ KEY_RWIN,0,0,0, 0,0,0,0,
};

static int MapNativeKey(int raw) {
	int key = raw >= 0 && raw < Array_Elems(key_map) ? key_map[raw] : 0;
	if (!key) Platform_Log2("Unknown key: %i (%h)", &raw, &raw);
	return key;
}

void Window_ProcessEvents(void) {
	CCEvent event;
	int key;
	
	while (Events_Pull(&event))
	{
		switch (event.type)
		{
		case CC_MOUSE_SCROLL:
			Mouse_ScrollWheel(event.v1.f32);
			break;
		case CC_MOUSE_DOWN:
			Input_SetPressed(event.v1.i32);
			break;
		case CC_MOUSE_UP:
			Input_SetReleased(event.v1.i32);
			break; 
		case CC_MOUSE_MOVE:
			Pointer_SetPosition(0, event.v1.i32, event.v2.i32);
			break;
		case CC_KEY_DOWN:
			key = MapNativeKey(event.v1.i32);
			if (key) Input_SetPressed(key);
			break; 
		case CC_KEY_UP:
			key = MapNativeKey(event.v1.i32);
			if (key) Input_SetReleased(key);
			break;
		case CC_KEY_INPUT:
			Event_RaiseInt(&InputEvents.Press, event.v1.i32);
			break;
		case CC_WIN_RESIZED:
			WindowInfo.Width  = event.v1.i32;
			WindowInfo.Height = event.v2.i32;
			Event_RaiseVoid(&WindowEvents.Resized);
			break;
		case CC_WIN_FOCUS:
			WindowInfo.Focused = event.v1.i32;
			Event_RaiseVoid(&WindowEvents.FocusChanged);
			break;
		case CC_WIN_REDRAW:
			Event_RaiseVoid(&WindowEvents.RedrawNeeded);
			break;
		case CC_WIN_QUIT:
			WindowInfo.Exists = false;
			Event_RaiseVoid(&WindowEvents.Closing);
			break;
		}
	}
}

static void Cursor_GetRawPos(int* x, int* y) {
	BPoint where;
	uint32 buttons;
	
	win_handle->Lock();
	view_handle->GetMouse(&where, &buttons, false);
	win_handle->Unlock();
	
	// TODO: Should checkQueue be true
	*x = (int)where.x;
	*y = (int)where.y;
}
void Cursor_SetPosition(int x, int y) {
	// https://discourse.libsdl.org/t/sdl-mouse-bug/597/11
	BRect frame = win_handle->Frame();
	set_mouse_position(frame.left + x, frame.top + y);
}

static void Cursor_DoSetVisible(cc_bool visible) {
	if (visible) {
		app_handle->ShowCursor();
	} else {
		app_handle->HideCursor();
	}
}

static void ShowDialogCore(const char* title, const char* msg) {
	BAlert* alert = new BAlert(title, msg, "OK");
	// doesn't show title by default
	alert->SetLook(B_TITLED_WINDOW_LOOK);
	alert->Go();
}

static BFilePanel* open_panel;
static OpenFileDialogCallback open_callback;
static const char* const* open_filters;

class CC_BRefFilter : public BRefFilter
{
public:
	CC_BRefFilter() : BRefFilter() { }
	
	bool Filter(const entry_ref* ref, BNode* node, stat_beos* st, const char* filetype) override {
		if (node->IsDirectory()) return true;
		
		BPath path(ref);
		cc_string str = String_FromReadonly(path.Path());
		
		for (int i = 0; open_filters[i]; i++)
		{
			cc_string ext = String_FromReadonly(open_filters[i]);
			if (String_CaselessEnds(&str, &ext)) return true;
		}
		return false;
	}
};

static void CallOpenFileCallback(const char* rawPath) {
	cc_string path; char pathBuffer[1024];
	String_InitArray(path, pathBuffer);
	if (!open_callback) return;
	
	String_AppendUtf8(&path, rawPath, String_Length(rawPath));
	open_callback(&path);
	open_callback = NULL;
}

cc_result Window_OpenFileDialog(const struct OpenFileDialogArgs* args) {
	if (!open_panel) {
		open_panel = new BFilePanel(B_OPEN_PANEL);
		open_panel->SetRefFilter(new CC_BRefFilter());
		// NOTE: the CC_BRefFilter is NOT owned by the BFilePanel,
		//  so this is technically a memory leak.. but meh
	}
	
	open_callback = args->Callback;
	open_filters  = args->filters;
	open_panel->Show();
	return 0;
}

static BBitmap* win_framebuffer;
void Window_AllocFramebuffer(struct Bitmap* bmp) {
	// right/bottom coordinates are inclusive of the coordinates,
	//  so need to subtract 1 to end up with correct width/height
	BRect bounds(0, 0, bmp->width - 1, bmp->height - 1);
	
	win_framebuffer = new BBitmap(bounds, B_RGB32);
	bmp->scan0 = (BitmapCol*)win_framebuffer->Bits();
}

void Window_DrawFramebuffer(Rect2D r) {
	// TODO rect should maybe subtract -1 too ????
	BRect rect(r.X, r.Y, r.X + r.Width, r.Y + r.Height);
	win_handle->Lock();
	view_handle->DrawBitmap(win_framebuffer, rect, rect);
	win_handle->Unlock();
}

void Window_FreeFramebuffer(struct Bitmap* bmp) {
	delete win_framebuffer;
	bmp->scan0 = NULL;
}

void Window_OpenKeyboard(struct OpenKeyboardArgs* args) { }
void Window_SetKeyboardText(const cc_string* text) { }
void Window_CloseKeyboard(void) {  }

void Window_EnableRawMouse(void)  { DefaultEnableRawMouse(); }
void Window_UpdateRawMouse(void)  { DefaultUpdateRawMouse(); }
void Window_DisableRawMouse(void) { DefaultDisableRawMouse(); }


/*########################################################################################################################*
*-----------------------------------------------------OpenGL context------------------------------------------------------*
*#########################################################################################################################*/
#if defined CC_BUILD_GL && !defined CC_BUILD_EGL
static cc_bool win_vsync;

void GLContext_Create(void) {
	view_3D->LockGL();
}

void GLContext_Update(void) { 
	// it's necessary to call UnlockGL then LockGL, otherwise resizing doesn't work
	//  (backbuffer rendering is performed to doesn't get resized)
	//  https://github.com/anholt/mesa/blob/01e511233b24872b08bff862ff692dfb5b22c1f4/src/gallium/targets/haiku-softpipe/SoftwareRenderer.cpp#L120..L127
	// might be fixed in newer MESA though?
	view_3D->UnlockGL();
	view_3D->LockGL();
}

cc_bool GLContext_TryRestore(void) { return true; }
void GLContext_Free(void) {
	view_3D->UnlockGL();
}

void* GLContext_GetAddress(const char* function) {
	return view_3D->GetGLProcAddress(function);
}

cc_bool GLContext_SwapBuffers(void) {
	view_3D->SwapBuffers(win_vsync);
	return true;
}

void GLContext_SetFpsLimit(cc_bool vsync, float minFrameMs) {
	win_vsync = vsync;
}
void GLContext_GetApiInfo(cc_string* info) { }
#endif
#endif
