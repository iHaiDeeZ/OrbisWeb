// Wisp: WPE's platform on the PS4 (ui/platform.h).
#include "platform.h"

#include "../common/wisp.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <glib-object.h>
#include <map>
#include <vector>
#include <wpe/wpe-platform.h>

namespace {

std::function<void()> g_redraw;
wisp::TextRequest g_text_request;
std::string g_cursor = "default";
uint64_t g_frame_serial = 0;
WPEModifiers g_modifiers = static_cast<WPEModifiers>(0);   // the USB keyboard's, as the UI last said

void redraw()
{
    if (g_redraw)
        g_redraw();
}

} // namespace

// --- GObject types ------------------------------------------------------------------------------------

G_BEGIN_DECLS

#define WISP_TYPE_DISPLAY (wisp_display_get_type())
G_DECLARE_FINAL_TYPE(WispDisplay, wisp_display, WISP, DISPLAY, WPEDisplay)
#define WISP_TYPE_VIEW (wisp_view_get_type())
G_DECLARE_FINAL_TYPE(WispView, wisp_view, WISP, VIEW, WPEView)
#define WISP_TYPE_TOPLEVEL (wisp_toplevel_get_type())
G_DECLARE_FINAL_TYPE(WispToplevel, wisp_toplevel, WISP, TOPLEVEL, WPEToplevel)
#define WISP_TYPE_SCREEN (wisp_screen_get_type())
G_DECLARE_FINAL_TYPE(WispScreen, wisp_screen, WISP, SCREEN, WPEScreen)
#define WISP_TYPE_KEYMAP (wisp_keymap_get_type())
G_DECLARE_FINAL_TYPE(WispKeymap, wisp_keymap, WISP, KEYMAP, WPEKeymap)
#define WISP_TYPE_INPUT_METHOD (wisp_input_method_get_type())
G_DECLARE_FINAL_TYPE(WispInputMethod, wisp_input_method, WISP, INPUT_METHOD, WPEInputMethodContext)
#define WISP_TYPE_GAMEPAD (wisp_gamepad_get_type())
G_DECLARE_FINAL_TYPE(WispGamepad, wisp_gamepad, WISP, GAMEPAD, WPEGamepad)
#define WISP_TYPE_GAMEPAD_MANAGER (wisp_gamepad_manager_get_type())
G_DECLARE_FINAL_TYPE(WispGamepadManager, wisp_gamepad_manager, WISP, GAMEPAD_MANAGER, WPEGamepadManager)

G_END_DECLS

// --- Screen: the TV, 1920x1080 at 60 Hz -------------------------------------------------------------

struct _WispScreen {
    WPEScreen parent;
};
G_DEFINE_FINAL_TYPE(WispScreen, wisp_screen, WPE_TYPE_SCREEN)
static void wisp_screen_class_init(WispScreenClass *) { }
static void wisp_screen_init(WispScreen *) { }

// --- Keymap: events carry their keyval (and the key's X11 hardware code, for KeyboardEvent.code);
// translating a code to a keyval is WebDriver's need only, answered with the keyval itself ----------

struct _WispKeymap {
    WPEKeymap parent;
};
G_DEFINE_FINAL_TYPE(WispKeymap, wisp_keymap, WPE_TYPE_KEYMAP)

static gboolean keymap_entries_for_keyval(WPEKeymap *, guint keyval, WPEKeymapEntry **entries, guint *count)
{
    *entries = g_new0(WPEKeymapEntry, 1);
    (*entries)[0].keycode = keyval;
    (*entries)[0].group = 0;
    (*entries)[0].level = 0;
    *count = 1;
    return TRUE;
}

static gboolean keymap_translate(WPEKeymap *, guint keycode, WPEModifiers, int, guint *keyval, int *effective_group,
    int *level, WPEModifiers *consumed)
{
    if (keyval)
        *keyval = keycode;
    if (effective_group)
        *effective_group = 0;
    if (level)
        *level = 0;
    if (consumed)
        *consumed = static_cast<WPEModifiers>(0);
    return TRUE;
}

// (WebPageProxy::currentStateOfModifierKeys asks this; without it the call went through a null pointer)
static WPEModifiers keymap_modifiers(WPEKeymap *) { return g_modifiers; }

static void wisp_keymap_class_init(WispKeymapClass *klass)
{
    auto *keymap_class = WPE_KEYMAP_CLASS(klass);
    keymap_class->get_entries_for_keyval = keymap_entries_for_keyval;
    keymap_class->translate_keyboard_state = keymap_translate;
    keymap_class->get_modifiers = keymap_modifiers;
}
static void wisp_keymap_init(WispKeymap *) { }

// --- Input method: the PS4 keyboard for the page's text fields -----------------------------------------

struct _WispInputMethod {
    WPEInputMethodContext parent;
    gchar *surrounding;      // the field's text, as the page last said
    guint cursor;            // in characters
    bool open;
};
G_DEFINE_FINAL_TYPE(WispInputMethod, wisp_input_method, WPE_TYPE_INPUT_METHOD_CONTEXT)

static void im_focus_in(WPEInputMethodContext *context)
{
    auto *self = WISP_INPUT_METHOD(context);
    if (self->open || !g_text_request)
        return;
    const WPEInputPurpose purpose = wpe_input_method_context_get_input_purpose(context);
    const bool password = purpose == WPE_INPUT_PURPOSE_PASSWORD || purpose == WPE_INPUT_PURPOSE_PIN;
    const bool url = purpose == WPE_INPUT_PURPOSE_URL;
    self->open = true;
    g_object_ref(self);
    g_text_request(self->surrounding ? self->surrounding : "", password, url,
        [self](bool done, const std::string &text) {
            self->open = false;
            if (done) {
                // the whole field becomes what was typed: delete what it had, commit the new text
                const glong length = self->surrounding ? g_utf8_strlen(self->surrounding, -1) : 0;
                if (length)
                    g_signal_emit_by_name(self, "delete-surrounding", -static_cast<int>(self->cursor), static_cast<guint>(length));
                g_signal_emit_by_name(self, "committed", text.c_str());
            }
            g_object_unref(self);
        });
}

static void im_focus_out(WPEInputMethodContext *) { }

static void im_set_surrounding(WPEInputMethodContext *context, const gchar *text, guint length, guint cursor, guint)
{
    auto *self = WISP_INPUT_METHOD(context);
    g_free(self->surrounding);
    self->surrounding = g_strndup(text, length);
    self->cursor = static_cast<guint>(g_utf8_pointer_to_offset(self->surrounding, self->surrounding + std::min<guint>(cursor, length)));
}

static void im_get_preedit(WPEInputMethodContext *, gchar **text, GList **underlines, guint *cursor)
{
    if (text)
        *text = g_strdup("");
    if (underlines)
        *underlines = nullptr;
    if (cursor)
        *cursor = 0;
}

static gboolean im_filter_key(WPEInputMethodContext *, WPEEvent *) { return FALSE; }
static void im_reset(WPEInputMethodContext *) { }
static void im_cursor_area(WPEInputMethodContext *, int, int, int, int) { }

static void im_finalize(GObject *object)
{
    g_free(WISP_INPUT_METHOD(object)->surrounding);
    G_OBJECT_CLASS(wisp_input_method_parent_class)->finalize(object);
}

static void wisp_input_method_class_init(WispInputMethodClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = im_finalize;
    auto *im_class = WPE_INPUT_METHOD_CONTEXT_CLASS(klass);
    im_class->focus_in = im_focus_in;
    im_class->focus_out = im_focus_out;
    im_class->set_surrounding = im_set_surrounding;
    im_class->get_preedit_string = im_get_preedit;
    im_class->filter_key_event = im_filter_key;
    im_class->reset = im_reset;
    im_class->set_cursor_area = im_cursor_area;
}
static void wisp_input_method_init(WispInputMethod *) { }

// --- Toplevel: one per view, the size of the content area ------------------------------------------

struct _WispToplevel {
    WPEToplevel parent;
    int width, height;
};
G_DEFINE_FINAL_TYPE(WispToplevel, wisp_toplevel, WPE_TYPE_TOPLEVEL)

static WPEScreen *toplevel_get_screen(WPEToplevel *toplevel);

static gboolean toplevel_resize(WPEToplevel *toplevel, int width, int height)
{
    auto *self = WISP_TOPLEVEL(toplevel);
    self->width = width;
    self->height = height;
    wpe_toplevel_resized(toplevel, width, height);
    wpe_toplevel_foreach_view(toplevel, [](WPEToplevel *, WPEView *view, gpointer data) -> gboolean {
        auto *self = static_cast<WispToplevel *>(data);
        wpe_view_resized(view, self->width, self->height);
        return FALSE;
    }, self);
    return TRUE;
}

static gboolean toplevel_set_fullscreen(WPEToplevel *toplevel, gboolean fullscreen)
{
    const WPEToplevelState state = wpe_toplevel_get_state(toplevel);
    wpe_toplevel_state_changed(toplevel, static_cast<WPEToplevelState>(fullscreen ? (state | WPE_TOPLEVEL_STATE_FULLSCREEN)
        : (state & ~WPE_TOPLEVEL_STATE_FULLSCREEN)));
    redraw();
    return TRUE;
}

static void wisp_toplevel_class_init(WispToplevelClass *klass)
{
    auto *toplevel_class = WPE_TOPLEVEL_CLASS(klass);
    toplevel_class->get_screen = toplevel_get_screen;
    toplevel_class->resize = toplevel_resize;
    toplevel_class->set_fullscreen = toplevel_set_fullscreen;
}
static void wisp_toplevel_init(WispToplevel *self)
{
    self->width = 1920;
    self->height = 1080;
}

// --- View: keeps the newest shared-memory frame ---------------------------------------------------------

// A view's frames: each one is copied as it comes and its buffer given back to WebKit at once, so the
// web process paints with two buffers (WebKit's AcceleratedSurface, patched: each 7.5 MiB buffer is
// mapped in both processes, out of the flexible memory pool they share). The copy is in the UI's heap
// (direct memory), and only what WebKit says changed (its damage rectangles) is copied into it: every
// buffer holds the whole frame, the copy holds the frame before. "Rendered" (WebKit's frame pacing)
// is said when the frame is on the screen, and at once for a hidden tab's, whose buffers the web
// process then gives back (a second after the tab is hidden: AcceleratedSurface, patched); the copy
// shows the tab again until its next frame comes.
struct _WispView {
    WPEView parent;
    WPEBuffer *unshown;              // the newest frame's buffer, until its frame is on the screen
    std::vector<uint8_t> *pixels;    // the newest frame (BGRA, premultiplied)
    int width, height, stride;
    uint64_t serial;
    int damage_y0, damage_y1;        // the rows changed since the browser last asked (platform_take_damage)
    bool picture;                    // pixels hold a saved picture (a tab woken from sleep): the page's
                                     // first frame is copied whole, whatever its damage says
};
G_DEFINE_FINAL_TYPE(WispView, wisp_view, WPE_TYPE_VIEW)

static gboolean view_render_buffer(WPEView *view, WPEBuffer *buffer, const WPERectangle *damage, guint n_damage, GError **error)
{
    if (!WPE_IS_BUFFER_SHM(buffer)) {
        static bool told = false;
        if (!told) {
            told = true;
            wisp_log("platform: the web process sent a %s buffer: only shared-memory frames are shown", G_OBJECT_TYPE_NAME(buffer));
        }
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "OrbisWeb shows shared-memory buffers only");
        return FALSE;
    }
    auto *self = WISP_VIEW(view);
    auto *shm = WPE_BUFFER_SHM(buffer);
    const int width = wpe_buffer_get_width(buffer), height = wpe_buffer_get_height(buffer);
    const int stride = static_cast<int>(wpe_buffer_shm_get_stride(shm));
    gsize size = 0;
    const auto *data = static_cast<const uint8_t *>(g_bytes_get_data(wpe_buffer_shm_get_data(shm), &size));
    if (!self->serial)
        wisp_log("platform: first page frame: %dx%d, stride %d, format %#x", width, height, stride, wpe_buffer_shm_get_format(shm));
    if (!data || width <= 0 || height <= 0 || size < static_cast<gsize>(stride) * height) {
        g_set_error_literal(error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "a frame without its pixels");
        return FALSE;
    }
    const size_t bytes = static_cast<size_t>(stride) * height;
    static const bool partial = wisp_setting_int("partial_frames", 1) != 0;   // (0: whole frames)
    const bool whole = !partial || !damage || !n_damage || self->pixels->size() != bytes || self->width != width
        || self->height != height || self->stride != stride || self->picture;
    self->picture = false;
    int y0 = 0, y1 = height;
    if (whole) {
        self->pixels->assign(data, data + bytes);
    } else {
        y0 = height;
        y1 = 0;
        uint8_t *to = self->pixels->data();
        for (guint i = 0; i < n_damage; i++) {
            const int x = std::clamp(damage[i].x, 0, width), x_end = std::clamp(damage[i].x + damage[i].width, 0, width);
            const int top = std::clamp(damage[i].y, 0, height), bottom = std::clamp(damage[i].y + damage[i].height, 0, height);
            if (x >= x_end || top >= bottom)
                continue;
            for (int row = top; row < bottom; row++) {
                const size_t at = static_cast<size_t>(row) * stride + static_cast<size_t>(x) * 4;
                memcpy(to + at, data + at, static_cast<size_t>(x_end - x) * 4);
            }
            y0 = std::min(y0, top);
            y1 = std::max(y1, bottom);
        }
    }
    if (y0 < y1) {
        self->damage_y0 = std::min(self->damage_y0, y0);
        self->damage_y1 = std::max(self->damage_y1, y1);
    }
    self->width = width;
    self->height = height;
    self->stride = stride;
    wpe_view_buffer_released(view, buffer);
    // a frame never shown (a newer one came first) counts as shown: one "rendered" per frame
    if (self->unshown) {
        wpe_view_buffer_rendered(view, self->unshown);
        g_clear_object(&self->unshown);
    }
    self->serial = ++g_frame_serial;
    if (!wpe_view_get_visible(view)) {
        // a hidden tab's frame: rendered now, and its buffer not kept (a hidden page gives its buffers
        // back, and a reference here kept one mapped: 7.5 MiB of the shared pool per hidden tab)
        wpe_view_buffer_rendered(view, buffer);
        return TRUE;
    }
    self->unshown = WPE_BUFFER(g_object_ref(buffer));
    redraw();
    return TRUE;
}

static void view_set_cursor_from_name(WPEView *, const char *name)
{
    if (name && g_cursor != name) {
        g_cursor = name;
        redraw();
    }
}

static gboolean view_can_be_mapped(WPEView *) { return TRUE; }

static void view_dispose(GObject *object)
{
    auto *self = WISP_VIEW(object);
    g_clear_object(&self->unshown);
    G_OBJECT_CLASS(wisp_view_parent_class)->dispose(object);
}

static void view_finalize(GObject *object)
{
    delete WISP_VIEW(object)->pixels;
    G_OBJECT_CLASS(wisp_view_parent_class)->finalize(object);
}

static void view_toplevel_changed(WPEView *view, GParamSpec *, gpointer)
{
    auto *toplevel = wpe_view_get_toplevel(view);
    if (!toplevel) {
        wpe_view_unmap(view);
        return;
    }
    int width = 0;
    int height = 0;
    wpe_toplevel_get_size(toplevel, &width, &height);
    if (width && height)
        wpe_view_resized(view, width, height);
    wpe_view_map(view);
}

static void view_constructed(GObject *object)
{
    G_OBJECT_CLASS(wisp_view_parent_class)->constructed(object);
    g_signal_connect(object, "notify::toplevel", G_CALLBACK(view_toplevel_changed), nullptr);
}

static void wisp_view_class_init(WispViewClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = view_dispose;
    G_OBJECT_CLASS(klass)->finalize = view_finalize;
    G_OBJECT_CLASS(klass)->constructed = view_constructed;
    auto *view_class = WPE_VIEW_CLASS(klass);
    view_class->render_buffer = view_render_buffer;
    view_class->set_cursor_from_name = view_set_cursor_from_name;
    view_class->can_be_mapped = view_can_be_mapped;
}
static void wisp_view_init(WispView *self)
{
    self->pixels = new std::vector<uint8_t>();
    self->damage_y0 = INT32_MAX;
    self->damage_y1 = 0;
}

// --- Gamepads (v00.30): the controller as a page's standard gamepad ----------------------------------
// WebKit's GamepadProviderWPE makes a manager (display_create_gamepad_manager) when the first page
// starts reading gamepads and lets it go when none does; the controller is its one device while it is
// connected. The UI's state each frame (platform_gamepad_update) goes out as WPE's events, only what
// changed, and only while WebKit monitors input (start_input_monitor).

namespace {
wisp::GamepadHooks g_gamepad_hooks;
wisp::GamepadState g_gamepad_state;              // the controller as the UI gave it last
WispGamepadManager *g_gamepad_manager = nullptr;  // WebKit's, while a page reads gamepads (not ours to keep)
WispGamepad *g_gamepad = nullptr;                 // the controller in it, while connected (ours)
constexpr int GAMEPAD_BUTTONS = 17, GAMEPAD_AXES = 4;
// As Chrome on Windows names a DualShock 4: pages that tell controllers apart read the vendor and
// product there (Sony 054c, the DualShock 4's second model 09cc)
constexpr const char *GAMEPAD_NAME = "Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 09cc)";
}

struct _WispGamepad {
    WPEGamepad parent;
    gboolean monitoring;
    double buttons[GAMEPAD_BUTTONS];   // as the page last got them (-1: not sent yet)
    double axes[GAMEPAD_AXES];
};
G_DEFINE_FINAL_TYPE(WispGamepad, wisp_gamepad, WPE_TYPE_GAMEPAD)

static void gamepad_forget_sent(WispGamepad *self)
{
    for (double &value : self->buttons)
        value = -1;
    for (double &value : self->axes)
        value = -2;
}

static void gamepad_start_input_monitor(WPEGamepad *gamepad)
{
    auto *self = WISP_GAMEPAD(gamepad);
    if (!self->monitoring)
        wisp_log("gamepad: WebKit reads the controller's input");
    self->monitoring = TRUE;
    gamepad_forget_sent(self);   // (the whole state goes out at the next update)
}

static void gamepad_stop_input_monitor(WPEGamepad *gamepad)
{
    auto *self = WISP_GAMEPAD(gamepad);
    if (self->monitoring)
        wisp_log("gamepad: WebKit stops reading the controller's input (its events still go out: see platform_gamepad_update)");
    self->monitoring = FALSE;
}

static gboolean gamepad_has_rumble(WPEGamepad *) { return TRUE; }

static gboolean gamepad_rumble(WPEGamepad *, gdouble strong, gdouble weak, guint ms)
{
    if (g_gamepad_hooks.rumble)
        g_gamepad_hooks.rumble(strong, weak, ms);
    return TRUE;
}

static void wisp_gamepad_class_init(WispGamepadClass *klass)
{
    auto *gamepad_class = WPE_GAMEPAD_CLASS(klass);
    gamepad_class->start_input_monitor = gamepad_start_input_monitor;
    gamepad_class->stop_input_monitor = gamepad_stop_input_monitor;
    gamepad_class->has_rumble = gamepad_has_rumble;
    gamepad_class->rumble = gamepad_rumble;
}
static void wisp_gamepad_init(WispGamepad *self) { gamepad_forget_sent(self); }

struct _WispGamepadManager {
    WPEGamepadManager parent;
};
G_DEFINE_FINAL_TYPE(WispGamepadManager, wisp_gamepad_manager, WPE_TYPE_GAMEPAD_MANAGER)

static void gamepad_add(WispGamepadManager *manager)
{
    const char *name = wisp_setting("gamepad_name", GAMEPAD_NAME);
    g_gamepad = WISP_GAMEPAD(g_object_new(WISP_TYPE_GAMEPAD, "name", *name ? name : GAMEPAD_NAME, nullptr));
    wpe_gamepad_manager_add_device(WPE_GAMEPAD_MANAGER(manager), WPE_GAMEPAD(g_gamepad));
    wisp_log("gamepad: the controller is a page's gamepad now (\"%s\")", wpe_gamepad_get_name(WPE_GAMEPAD(g_gamepad)));
}

static void gamepad_remove(WispGamepadManager *manager)
{
    if (!g_gamepad)
        return;
    if (manager)
        wpe_gamepad_manager_remove_device(WPE_GAMEPAD_MANAGER(manager), WPE_GAMEPAD(g_gamepad));
    g_clear_object(&g_gamepad);
}

static void gamepad_manager_dispose(GObject *object)
{
    if (g_gamepad_manager == WISP_GAMEPAD_MANAGER(object)) {
        g_gamepad_manager = nullptr;
        gamepad_remove(nullptr);   // (the manager's own reference goes with it)
        wisp_log("gamepad: no page reads gamepads any more");
        if (g_gamepad_hooks.in_use)
            g_gamepad_hooks.in_use(false);
    }
    G_OBJECT_CLASS(wisp_gamepad_manager_parent_class)->dispose(object);
}

static void wisp_gamepad_manager_class_init(WispGamepadManagerClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = gamepad_manager_dispose;
}
static void wisp_gamepad_manager_init(WispGamepadManager *) { }

static WPEGamepadManager *display_create_gamepad_manager(WPEDisplay *)
{
    auto *manager = WISP_GAMEPAD_MANAGER(g_object_new(WISP_TYPE_GAMEPAD_MANAGER, nullptr));
    if (g_gamepad_manager)   // (WebKit makes one at a time; an older one is no longer ours to fill)
        gamepad_remove(g_gamepad_manager);
    g_gamepad_manager = manager;
    wisp_log("gamepad: a page reads gamepads (the controller %s)", g_gamepad_state.connected ? "is connected" : "is not connected");
    if (g_gamepad_state.connected)
        gamepad_add(manager);
    if (g_gamepad_hooks.in_use)
        g_gamepad_hooks.in_use(true);
    return WPE_GAMEPAD_MANAGER(manager);
}

// --- Display -------------------------------------------------------------------------------------------

struct _WispDisplay {
    WPEDisplay parent;
    WPEScreen *screen;
    WPEKeymap *keymap;
};
G_DEFINE_FINAL_TYPE(WispDisplay, wisp_display, WPE_TYPE_DISPLAY)

static WPEScreen *display_screen(WispDisplay *self)
{
    if (!self->screen) {
        self->screen = WPE_SCREEN(g_object_new(WISP_TYPE_SCREEN, "id", 1u, nullptr));
        wpe_screen_set_position(self->screen, 0, 0);
        wpe_screen_set_size(self->screen, 1920, 1080);
        wpe_screen_set_physical_size(self->screen, 1210, 680);   // a 55" TV, in mm
        wpe_screen_set_scale(self->screen, 1.0);
        wpe_screen_set_refresh_rate(self->screen, 60000);         // mHz
    }
    return self->screen;
}

static WPEScreen *toplevel_get_screen(WPEToplevel *toplevel)
{
    return display_screen(WISP_DISPLAY(wpe_toplevel_get_display(toplevel)));
}

static gboolean display_connect(WPEDisplay *, GError **) { return TRUE; }

static WPEView *display_create_view(WPEDisplay *display)
{
    return WPE_VIEW(g_object_new(WISP_TYPE_VIEW, "display", display, nullptr));
}

static WPEToplevel *display_create_toplevel(WPEDisplay *display, guint max_views)
{
    return WPE_TOPLEVEL(g_object_new(WISP_TYPE_TOPLEVEL, "display", display, "max-views", max_views, nullptr));
}

static gpointer display_get_egl_display(WPEDisplay *, GError **error)
{
    // The UI draws on the CPU; GL (Mesa on Zink) lives in the web process only.
    g_set_error_literal(error, WPE_EGL_ERROR, WPE_EGL_ERROR_NOT_AVAILABLE, "no EGL in OrbisWeb's UI process");
    return nullptr;
}

static WPEKeymap *display_get_keymap(WPEDisplay *display)
{
    auto *self = WISP_DISPLAY(display);
    if (!self->keymap)
        self->keymap = WPE_KEYMAP(g_object_new(WISP_TYPE_KEYMAP, nullptr));
    return self->keymap;
}

static guint display_get_n_screens(WPEDisplay *) { return 1; }
static WPEScreen *display_get_screen(WPEDisplay *display, guint) { return display_screen(WISP_DISPLAY(display)); }

static WPEInputMethodContext *display_create_input_method(WPEDisplay *, WPEView *view)
{
    return WPE_INPUT_METHOD_CONTEXT(g_object_new(WISP_TYPE_INPUT_METHOD, "view", view, nullptr));
}

static void wisp_display_class_init(WispDisplayClass *klass)
{
    auto *display_class = WPE_DISPLAY_CLASS(klass);
    display_class->connect = display_connect;
    display_class->create_view = display_create_view;
    display_class->create_toplevel = display_create_toplevel;
    display_class->get_egl_display = display_get_egl_display;
    display_class->get_keymap = display_get_keymap;
    display_class->get_n_screens = display_get_n_screens;
    display_class->get_screen = display_get_screen;
    display_class->create_input_method_context = display_create_input_method;
    display_class->create_gamepad_manager = display_create_gamepad_manager;
}
static void wisp_display_init(WispDisplay *) { }

// --- The UI's side -------------------------------------------------------------------------------------

namespace wisp {

WPEDisplay *platform_display()
{
    static WPEDisplay *display = nullptr;
    if (!display) {
        display = WPE_DISPLAY(g_object_new(WISP_TYPE_DISPLAY, nullptr));
        GError *error = nullptr;
        if (!wpe_display_connect(display, &error)) {
            wisp_log("platform: display: %s", error ? error->message : "?");
            g_clear_error(&error);
        }
        wpe_display_set_primary(display);
        wisp_log("platform: PS4 display ready (1920x1080, shared-memory frames)");
    }
    return display;
}

bool platform_frame(WPEView *view, Frame &frame)
{
    if (!view || !WISP_IS_VIEW(view))
        return false;
    auto *self = WISP_VIEW(view);
    if (self->pixels->empty())
        return false;
    frame.pixels = self->pixels->data();
    frame.width = self->width;
    frame.height = self->height;
    frame.stride = self->stride;
    frame.serial = self->serial;
    return true;
}

bool platform_take_damage(WPEView *view, int &y0, int &y1)
{
    if (!view || !WISP_IS_VIEW(view))
        return false;
    auto *self = WISP_VIEW(view);
    if (self->damage_y0 >= self->damage_y1)
        return false;
    y0 = self->damage_y0;
    y1 = self->damage_y1;
    self->damage_y0 = INT32_MAX;
    self->damage_y1 = 0;
    return true;
}

void platform_show_picture(WPEView *view, std::vector<uint8_t> &&pixels, int width, int height, int stride)
{
    if (!view || !WISP_IS_VIEW(view) || width <= 0 || height <= 0 || pixels.size() < static_cast<size_t>(stride) * height)
        return;
    auto *self = WISP_VIEW(view);
    *self->pixels = std::move(pixels);
    self->width = width;
    self->height = height;
    self->stride = stride;
    self->serial = ++g_frame_serial;
    self->damage_y0 = 0;
    self->damage_y1 = height;
    self->picture = true;
    redraw();
}

void platform_frame_shown(WPEView *view)
{
    if (!view || !WISP_IS_VIEW(view))
        return;
    auto *self = WISP_VIEW(view);
    if (!self->unshown)
        return;
    WPEBuffer *buffer = self->unshown;
    self->unshown = nullptr;
    wpe_view_buffer_rendered(view, buffer);
    g_object_unref(buffer);
}

void platform_set_content_size(WPEView *view, int width, int height)
{
    auto *toplevel = view ? wpe_view_get_toplevel(view) : nullptr;
    if (!toplevel)
        return;
    int w, h;
    wpe_toplevel_get_size(toplevel, &w, &h);
    if (w != width || h != height)
        wpe_toplevel_resize(toplevel, width, height);
}

void platform_leave_fullscreen(WPEView *view)
{
    if (!view)
        return;
    if (WPEToplevel *toplevel = wpe_view_get_toplevel(view); toplevel && (wpe_toplevel_get_state(toplevel) & WPE_TOPLEVEL_STATE_FULLSCREEN))
        wpe_toplevel_unfullscreen(toplevel);
}

void platform_set_active(WPEView *view, bool active)
{
    if (!view)
        return;
    auto *toplevel = wpe_view_get_toplevel(view);
    if (toplevel) {
        const WPEToplevelState state = wpe_toplevel_get_state(toplevel);
        wpe_toplevel_state_changed(toplevel, static_cast<WPEToplevelState>(active ? (state | WPE_TOPLEVEL_STATE_ACTIVE)
            : (state & ~WPE_TOPLEVEL_STATE_ACTIVE)));
    }
    wpe_view_set_visible(view, active);
    if (active)
        wpe_view_focus_in(view);
    else
        wpe_view_focus_out(view);
    // hidden: its last frame is not waited for, nor its buffer kept (view_render_buffer)
    if (!active && WISP_IS_VIEW(view)) {
        auto *self = WISP_VIEW(view);
        if (self->unshown) {
            wpe_view_buffer_rendered(view, self->unshown);
            g_clear_object(&self->unshown);
        }
    }
}

void platform_set_text_request(TextRequest request) { g_text_request = std::move(request); }
void platform_set_redraw(std::function<void()> redraw) { g_redraw = std::move(redraw); }
const std::string &platform_cursor_name() { return g_cursor; }
void platform_set_modifiers(uint32_t modifiers) { g_modifiers = static_cast<WPEModifiers>(modifiers); }

std::string platform_clipboard_text()
{
    WPEClipboard *clipboard = wpe_display_get_clipboard(platform_display());
    for (const char *format : { "text/plain;charset=utf-8", "text/plain" }) {
        gsize size = 0;
        if (char *text = wpe_clipboard_read_text(clipboard, format, &size)) {
            std::string result(text, size);
            g_free(text);
            return result;
        }
    }
    return {};
}

void platform_set_clipboard_text(const std::string &text)
{
    WPEClipboardContent *content = wpe_clipboard_content_new();
    wpe_clipboard_content_set_text(content, text.c_str());
    wpe_clipboard_set_content(wpe_display_get_clipboard(platform_display()), content);
    wpe_clipboard_content_unref(content);
}

void platform_set_gamepad_hooks(GamepadHooks hooks) { g_gamepad_hooks = std::move(hooks); }

// (v00.41) Whether WebKit finds a page for the controller's input as it does (WPEWebViewPlatform.cpp: a
// visible view with the input focus in an active toplevel), said in ui.log when a page takes the controller:
// without one, WebKit sends the page nothing
void platform_gamepad_check(WPEView *expected)
{
    int toplevels = 0, active = 0;
    WPEView *found = nullptr;
    GList *list = wpe_toplevel_list();
    for (GList *item = list; item; item = item->next) {
        auto *toplevel = WPE_TOPLEVEL(item->data);
        toplevels++;
        if (!(wpe_toplevel_get_state(toplevel) & WPE_TOPLEVEL_STATE_ACTIVE))
            continue;
        active++;
        wpe_toplevel_foreach_view(toplevel, +[](WPEToplevel *, WPEView *view, gpointer data) -> gboolean {
            if (wpe_view_get_visible(view) && wpe_view_get_has_focus(view)) {
                *static_cast<WPEView **>(data) = view;
                return TRUE;
            }
            return FALSE;
        }, &found);
        if (found)
            break;
    }
    g_list_free(list);
    if (found)
        wisp_log("gamepad: WebKit's page for its input: %s (%d toplevels, %d active); WebKit %s the controller's input",
            found == expected ? "the tab on show" : "ANOTHER view", toplevels, active,
            g_gamepad && g_gamepad->monitoring ? "reads" : "does not say it reads");
    else
        wisp_log("gamepad: WebKit finds NO page for its input (%d toplevels, %d active; the tab's view visible %d, focused %d)",
            toplevels, active, expected ? wpe_view_get_visible(expected) : -1, expected ? wpe_view_get_has_focus(expected) : -1);
}

bool platform_gamepads_in_use() { return g_gamepad_manager != nullptr; }

void platform_gamepad_update(const GamepadState &state)
{
    g_gamepad_state = state;
    if (!g_gamepad_manager)
        return;
    // the controller connected or gone while a page reads gamepads: its gamepadconnected / disconnected
    if (state.connected && !g_gamepad)
        gamepad_add(g_gamepad_manager);
    else if (!state.connected && g_gamepad)
        gamepad_remove(g_gamepad_manager);
    // (v00.41) Sent whether or not WebKit says it monitors input: its gamepad listens to these events all the
    // time and hands them to the page it finds for gamepad input. Its monitoring stops when a view goes
    // inactive with no other active (a tab switch hides the old tab before showing the new one) and starts
    // again only when a view becomes active while a page reads gamepads; a page that started reading later
    // (Zelda's EmulatorJS in v00.40, after its web process had restarted) got nothing: its gamepad list stayed
    // empty while the UI showed "The page has the controller"
    if (!g_gamepad)
        return;
    auto *gamepad = WPE_GAMEPAD(g_gamepad);
    for (int i = 0; i < GAMEPAD_BUTTONS; i++) {
        const double value = std::clamp(state.buttons[i], 0.0, 1.0);
        if (value == g_gamepad->buttons[i])
            continue;
        g_gamepad->buttons[i] = value;
        if (i == WPE_GAMEPAD_BUTTON_LEFT_SHOULDER_BACK || i == WPE_GAMEPAD_BUTTON_RIGHT_SHOULDER_BACK)
            wpe_gamepad_button_value_event(gamepad, static_cast<WPEGamepadButton>(i), value);   // (L2, R2: how far, WebKit patch 0030)
        else
            wpe_gamepad_button_event(gamepad, static_cast<WPEGamepadButton>(i), value > 0.5);
    }
    for (int i = 0; i < GAMEPAD_AXES; i++) {
        const double value = std::clamp(state.axes[i], -1.0, 1.0);
        if (value == g_gamepad->axes[i])
            continue;
        g_gamepad->axes[i] = value;
        wpe_gamepad_axis_event(gamepad, static_cast<WPEGamepadAxis>(i), value);
    }
}

} // namespace wisp
