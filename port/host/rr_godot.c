/* Godot native extension: exposes the console as the class RoadRashConsole.
 *
 * Written against Godot's C interface (gdextension_interface.h, dumped from the
 * installed Godot at build time), so it needs no binding library or extra
 * build tool. The class extends RefCounted:
 *
 *   load_rom(path: String) -> String     "" on success, else an error message
 *   reset()
 *   set_buttons(port: int, held: int)    PAD_* bits
 *   run_frame() -> bool                  false when halted or no ROM is loaded
 *   get_frame() -> PackedByteArray       get_frame_width() x get_frame_height() RGBA8
 *                                        (320 x 224 as loaded), empty before a ROM is loaded
 *   set_wide(on), get_wide() -> bool     the wide view (rr_console_set_wide): while it
 *                                        is in effect the frame is 398 * scale across
 *   set_sound(on), get_sound(), take_sound(rate) -> PackedByteArray
 *                                        the game's sound since the last call, as
 *                                        left/right pairs of 32-bit floats
 *   get_halt_message() -> String         "" while running
 *   get_ram() -> PackedByteArray         the 65,536 bytes of work RAM
 *   get_session() -> String              everything done since the ROM was loaded,
 *                                        by frame (rr_console.h); "" if there is none
 *   get_frame_count() -> int
 *
 * Extension-level state (the interface function pointers and names below) is
 * process-wide by nature: Godot loads the library once.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gdextension_interface.h"
#include "rr_console.h"
#include "rr_genesis.h"

#define CLASS_NAME "RoadRashConsole"
#define PROPERTY_USAGE_DEFAULT 6
/* Hash of PackedByteArray.resize in Godot's extension_api.json; hashes are kept
 * stable across versions for compatibility. */
#define PACKED_BYTE_ARRAY_RESIZE_HASH 848867239
/* Hash of Object.notification(what: int, reversed: bool), from the same file. */
#define OBJECT_NOTIFICATION_HASH 4023243586
#define NOTIFICATION_POSTINITIALIZE 0
#define MAX_ARGS 2

/* Storage big enough for any argument or return type used here (String and
 * StringName are one pointer, PackedByteArray two, on 64-bit builds). */
typedef union {
    int64_t i;
    GDExtensionBool b;
    uint8_t opaque[16];
    void *align;
} Value;

typedef struct {
    RrConsole *console;
} Instance;

typedef void (*MethodImpl)(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret);

typedef struct {
    const char *name;
    GDExtensionVariantType ret;
    int argc;
    GDExtensionVariantType arg_types[MAX_ARGS];
    const char *arg_names[MAX_ARGS];
    MethodImpl impl;
} Method;

static struct {
    GDExtensionClassLibraryPtr library;
    GDExtensionInterfaceClassdbRegisterExtensionClass6 register_class;
    GDExtensionInterfaceClassdbRegisterExtensionClassMethod register_method;
    GDExtensionInterfaceClassdbRegisterExtensionClassIntegerConstant register_constant;
    GDExtensionInterfaceClassdbConstructObject3 construct_object;
    GDExtensionInterfaceObjectMethodBindPtrcall method_ptrcall;
    GDExtensionMethodBindPtr object_notification;
    GDExtensionInterfaceObjectSetInstance set_instance;
    GDExtensionInterfaceStringNameNewWithLatin1Chars string_name_new;
    GDExtensionInterfaceStringNewWithUtf8Chars string_new;
    GDExtensionInterfaceStringToUtf8Chars string_to_utf8;
    GDExtensionInterfacePackedByteArrayOperatorIndex bytes_index;
    GDExtensionPtrConstructor bytes_new;
    GDExtensionPtrBuiltInMethod bytes_resize;
    GDExtensionPtrDestructor destroy_string, destroy_string_name, destroy_bytes;
    GDExtensionVariantFromTypeConstructorFunc to_variant[GDEXTENSION_VARIANT_TYPE_VARIANT_MAX];
    GDExtensionTypeFromVariantConstructorFunc from_variant[GDEXTENSION_VARIANT_TYPE_VARIANT_MAX];
} api;

/* A StringName for a C literal. Static names are owned by Godot and never freed. */
static void name_of(Value *out, const char *text) { api.string_name_new(out, text, 1); }

static void destroy_value(GDExtensionVariantType type, Value *value) {
    if (type == GDEXTENSION_VARIANT_TYPE_STRING) api.destroy_string(value);
    else if (type == GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY) api.destroy_bytes(value);
}

/* ---- methods, in pointer-call form ---- */

static void m_load_rom(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    char path[4096], error[512] = "";
    GDExtensionInt length = api.string_to_utf8(args[0], path, (GDExtensionInt)sizeof path - 1);
    if (length < 0) length = 0;
    if (length > (GDExtensionInt)sizeof path - 1) length = (GDExtensionInt)sizeof path - 1;
    path[length] = 0;
    RrConsole *opened = rr_console_open(path, error, sizeof error);
    if (opened != NULL) {
        rr_console_close(self->console);
        self->console = opened;
    }
    api.string_new(ret, error);
}

static void m_reset(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    (void)ret;
    if (self->console != NULL) rr_console_reset(self->console);
}

static void m_set_buttons(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)ret;
    int64_t port = *(const int64_t *)args[0], held = *(const int64_t *)args[1];
    if (self->console != NULL && port >= 0 && port < RR_CONSOLE_PORTS) {
        rr_console_set_buttons(self->console, (int)port, (unsigned)(held & 0xFF));
    }
}

static void m_run_frame(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(GDExtensionBool *)ret = self->console != NULL && rr_console_run_frame(self->console);
}

static void m_get_frame(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    api.bytes_new(ret, NULL);
    if (self->console == NULL) return;
    int scale = rr_console_scale(self->console);
    int64_t pixel_count = (int64_t)rr_console_scaled_width(self->console) * RR_CONSOLE_H * scale;
    int64_t size = pixel_count * 4, resized = 0;
    GDExtensionConstTypePtr resize_args[1] = {&size};
    api.bytes_resize(ret, resize_args, &resized, 1);
    uint8_t *out = api.bytes_index(ret, 0);
    const uint32_t *pixels = rr_console_scaled_frame(self->console);
    for (int64_t i = 0; i < pixel_count; i++) {
        out[i * 4 + 0] = (uint8_t)(pixels[i] >> 16);
        out[i * 4 + 1] = (uint8_t)(pixels[i] >> 8);
        out[i * 4 + 2] = (uint8_t)pixels[i];
        out[i * 4 + 3] = 255;
    }
}

static void m_get_ram(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    api.bytes_new(ret, NULL);
    if (self->console == NULL) return;
    int64_t size = 65536, resized = 0;
    GDExtensionConstTypePtr resize_args[1] = {&size};
    api.bytes_resize(ret, resize_args, &resized, 1);
    memcpy(api.bytes_index(ret, 0), rr_console_ram(self->console), (size_t)size);
}

static void m_get_session(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    char *text = self->console != NULL ? rr_console_session_text(self->console) : NULL;
    api.string_new(ret, text != NULL ? text : "");
    free(text);
}

static void m_set_sound(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)ret;
    if (self->console != NULL) rr_console_set_sound(self->console, *(const GDExtensionBool *)args[0] != 0);
}

static void m_get_sound(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(GDExtensionBool *)ret = self->console != NULL && rr_console_sound(self->console);
}

/* The sound made since the last call, at `rate` samples a second: left/right
 * pairs of 32-bit floats in the machine's byte order (little-endian on every
 * machine Godot runs on), for PackedByteArray.to_float32_array. */
#define SOUND_MAX_FRAMES 16384
static void m_take_sound(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    static float pairs[2 * SOUND_MAX_FRAMES];
    api.bytes_new(ret, NULL);
    if (self->console == NULL) return;
    size_t made = rr_console_take_sound(self->console, (double)*(const int64_t *)args[0], pairs, SOUND_MAX_FRAMES);
    if (made == 0) return;
    int64_t size = (int64_t)(made * 2 * sizeof(float)), resized = 0;
    GDExtensionConstTypePtr resize_args[1] = {&size};
    api.bytes_resize(ret, resize_args, &resized, 1);
    memcpy(api.bytes_index(ret, 0), pairs, (size_t)size);
}

static void m_set_scale(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)ret;
    int64_t scale = *(const int64_t *)args[0];
    if (self->console == NULL) return;
    if (scale < 1) scale = 1;
    if (scale > RR_CONSOLE_SCALE_MAX) scale = RR_CONSOLE_SCALE_MAX;
    rr_console_set_scale(self->console, (int)scale);
}

static int scale_of(const Instance *self) {
    return self->console != NULL ? rr_console_scale(self->console) : 1;
}

static void m_get_scale(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(int64_t *)ret = scale_of(self);
}

static void m_get_frame_width(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(int64_t *)ret = self->console != NULL ? rr_console_scaled_width(self->console) : RR_CONSOLE_W;
}

static void m_get_frame_height(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(int64_t *)ret = (int64_t)RR_CONSOLE_H * scale_of(self);
}

static void m_set_fine_road(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)ret;
    if (self->console != NULL) rr_console_set_fine_road(self->console, *(const GDExtensionBool *)args[0] != 0);
}

static void m_get_fine_road(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(GDExtensionBool *)ret = self->console == NULL || rr_console_fine_road(self->console);
}

static void m_set_fine_objects(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)ret;
    if (self->console != NULL) rr_console_set_fine_objects(self->console, *(const GDExtensionBool *)args[0] != 0);
}

static void m_get_fine_objects(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(GDExtensionBool *)ret = self->console == NULL || rr_console_fine_objects(self->console);
}

static void m_set_wide(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)ret;
    if (self->console != NULL) rr_console_set_wide(self->console, *(const GDExtensionBool *)args[0] != 0);
}

static void m_get_wide(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(GDExtensionBool *)ret = self->console != NULL && rr_console_wide(self->console);
}

static void m_set_soft_objects(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)ret;
    if (self->console != NULL) rr_console_set_soft_objects(self->console, *(const GDExtensionBool *)args[0] != 0);
}

static void m_get_soft_objects(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(GDExtensionBool *)ret = self->console != NULL && rr_console_soft_objects(self->console);
}

static void m_set_between(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)ret;
    int64_t mode = *(const int64_t *)args[0];
    if (self->console != NULL && mode >= 0 && mode <= 3) rr_console_set_between(self->console, (int)mode);
}

static void m_get_between(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(int64_t *)ret = self->console != NULL ? rr_console_between(self->console) : 0;
}

static void m_set_crisp_road(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)ret;
    if (self->console != NULL) rr_console_set_crisp_road(self->console, *(const GDExtensionBool *)args[0] != 0);
}

static void m_get_halt_message(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    const char *message = self->console != NULL ? rr_console_halt_message(self->console) : NULL;
    api.string_new(ret, message != NULL ? message : "");
}

static void m_get_frame_count(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(int64_t *)ret = self->console != NULL ? (int64_t)rr_console_frames(self->console) : 0;
}

static void m_set_cpu_speed(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)ret;
    int64_t percent = *(const int64_t *)args[0];
    if (self->console == NULL) return;
    if (percent < 0) percent = 0;
    if (percent > 1000000) percent = 1000000;
    rr_console_set_cpu_speed(self->console, (unsigned)percent);
}

static void m_get_cpu_speed(Instance *self, const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    (void)args;
    *(int64_t *)ret = self->console != NULL ? (int64_t)rr_console_cpu_speed(self->console) : 100;
}

#define T_NIL GDEXTENSION_VARIANT_TYPE_NIL
#define T_BOOL GDEXTENSION_VARIANT_TYPE_BOOL
#define T_INT GDEXTENSION_VARIANT_TYPE_INT
#define T_STRING GDEXTENSION_VARIANT_TYPE_STRING
#define T_BYTES GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY

static const Method methods[] = {
    {"load_rom", T_STRING, 1, {T_STRING}, {"path"}, m_load_rom},
    {"reset", T_NIL, 0, {0}, {0}, m_reset},
    {"set_buttons", T_NIL, 2, {T_INT, T_INT}, {"port", "held"}, m_set_buttons},
    {"run_frame", T_BOOL, 0, {0}, {0}, m_run_frame},
    {"get_frame", T_BYTES, 0, {0}, {0}, m_get_frame},
    {"get_halt_message", T_STRING, 0, {0}, {0}, m_get_halt_message},
    {"get_ram", T_BYTES, 0, {0}, {0}, m_get_ram},
    {"get_session", T_STRING, 0, {0}, {0}, m_get_session},
    {"get_frame_count", T_INT, 0, {0}, {0}, m_get_frame_count},
    {"set_cpu_speed", T_NIL, 1, {T_INT}, {"percent"}, m_set_cpu_speed},
    {"get_cpu_speed", T_INT, 0, {0}, {0}, m_get_cpu_speed},
    {"set_scale", T_NIL, 1, {T_INT}, {"scale"}, m_set_scale},
    {"get_scale", T_INT, 0, {0}, {0}, m_get_scale},
    {"get_frame_width", T_INT, 0, {0}, {0}, m_get_frame_width},
    {"get_frame_height", T_INT, 0, {0}, {0}, m_get_frame_height},
    {"set_crisp_road", T_NIL, 1, {T_BOOL}, {"on"}, m_set_crisp_road},
    {"set_fine_road", T_NIL, 1, {T_BOOL}, {"on"}, m_set_fine_road},
    {"get_fine_road", T_BOOL, 0, {0}, {0}, m_get_fine_road},
    {"set_fine_objects", T_NIL, 1, {T_BOOL}, {"on"}, m_set_fine_objects},
    {"get_fine_objects", T_BOOL, 0, {0}, {0}, m_get_fine_objects},
    {"set_soft_objects", T_NIL, 1, {T_BOOL}, {"on"}, m_set_soft_objects},
    {"set_wide", T_NIL, 1, {T_BOOL}, {"on"}, m_set_wide},
    {"get_wide", T_BOOL, 0, {0}, {0}, m_get_wide},
    {"set_sound", T_NIL, 1, {T_BOOL}, {"on"}, m_set_sound},
    {"get_sound", T_BOOL, 0, {0}, {0}, m_get_sound},
    {"take_sound", T_BYTES, 1, {T_INT}, {"rate"}, m_take_sound},
    {"get_soft_objects", T_BOOL, 0, {0}, {0}, m_get_soft_objects},
    {"set_between", T_NIL, 1, {T_INT}, {"mode"}, m_set_between},
    {"get_between", T_INT, 0, {0}, {0}, m_get_between},
};
#define METHOD_COUNT ((int)(sizeof methods / sizeof methods[0]))

static const struct {
    const char *name;
    int64_t value;
} constants[] = {
    {"WIDTH", RR_CONSOLE_W}, {"HEIGHT", RR_CONSOLE_H},
    {"PAD_UP", RR_PAD_UP}, {"PAD_DOWN", RR_PAD_DOWN}, {"PAD_LEFT", RR_PAD_LEFT},
    {"PAD_RIGHT", RR_PAD_RIGHT}, {"PAD_B", RR_PAD_B}, {"PAD_C", RR_PAD_C},
    {"PAD_A", RR_PAD_A}, {"PAD_START", RR_PAD_START},
};

/* ---- glue between Godot's two calling conventions and the methods ---- */

static void ptrcall(void *userdata, GDExtensionClassInstancePtr instance,
                    const GDExtensionConstTypePtr *args, GDExtensionTypePtr ret) {
    const Method *method = userdata;
    method->impl(instance, args, ret);
}

/* Call with Variant arguments: convert them to typed values, run the method,
 * and convert the result back. */
static void varcall(void *userdata, GDExtensionClassInstancePtr instance,
                    const GDExtensionConstVariantPtr *args, GDExtensionInt argc,
                    GDExtensionVariantPtr ret, GDExtensionCallError *error) {
    const Method *method = userdata;
    if (argc != method->argc) {
        error->error = argc < method->argc ? GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS
                                           : GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
        error->expected = method->argc;
        return;
    }
    Value values[MAX_ARGS];
    GDExtensionConstTypePtr typed[MAX_ARGS];
    memset(values, 0, sizeof values);
    for (int i = 0; i < method->argc; i++) {
        api.from_variant[method->arg_types[i]](&values[i], (GDExtensionVariantPtr)args[i]);
        typed[i] = &values[i];
    }
    Value result;
    memset(&result, 0, sizeof result);
    method->impl(instance, typed, &result);
    if (method->ret != T_NIL) {
        api.to_variant[method->ret](ret, &result);
        destroy_value(method->ret, &result);
    }
    for (int i = 0; i < method->argc; i++) destroy_value(method->arg_types[i], &values[i]);
    error->error = GDEXTENSION_CALL_OK;
}

/* ---- object lifetime ---- */

static GDExtensionObjectPtr create_instance(void *class_userdata, GDExtensionBool notify_postinitialize) {
    (void)class_userdata;
    Value base, class_name;
    name_of(&base, "RefCounted");
    name_of(&class_name, CLASS_NAME);
    /* construct_object3 hands back a RefCounted whose initial reference Godot
     * takes over when it wraps the object; the older construct_object2 left an
     * extra reference and the object was never freed. No instance binding is
     * needed for a class used only from scripts. */
    GDExtensionObjectPtr object = api.construct_object(&base);
    Instance *self = calloc(1, sizeof *self);
    api.set_instance(object, &class_name, self);
    if (notify_postinitialize) {
        /* Godot requires this notification after construction. */
        int64_t what = NOTIFICATION_POSTINITIALIZE;
        GDExtensionBool reversed = 0;
        GDExtensionConstTypePtr args[2] = {&what, &reversed};
        api.method_ptrcall(api.object_notification, object, args, NULL);
    }
    return object;
}

static void free_instance(void *class_userdata, GDExtensionClassInstancePtr instance) {
    (void)class_userdata;
    Instance *self = instance;
    if (self == NULL) return;
    rr_console_close(self->console);
    free(self);
}

/* ---- registration ---- */

static void property_info(GDExtensionPropertyInfo *info, GDExtensionVariantType type,
                          Value *name, Value *class_name, Value *hint, const char *text) {
    name_of(name, text);
    name_of(class_name, "");
    api.string_new(hint, "");
    info->type = type;
    info->name = name;
    info->class_name = class_name;
    info->hint = 0;
    info->hint_string = hint;
    info->usage = PROPERTY_USAGE_DEFAULT;
}

static void register_everything(void) {
    Value class_name, parent;
    name_of(&class_name, CLASS_NAME);
    name_of(&parent, "RefCounted");
    GDExtensionClassCreationInfo6 info;
    memset(&info, 0, sizeof info);
    info.is_exposed = 1;
    info.create_instance_func = create_instance;
    info.free_instance_func = free_instance;
    api.register_class(api.library, &class_name, &parent, &info);

    for (int m = 0; m < METHOD_COUNT; m++) {
        const Method *method = &methods[m];
        Value name, names[MAX_ARGS + 1], classes[MAX_ARGS + 1], hints[MAX_ARGS + 1];
        GDExtensionPropertyInfo arg_info[MAX_ARGS], ret_info;
        GDExtensionClassMethodArgumentMetadata metadata[MAX_ARGS] = {0};
        name_of(&name, method->name);
        for (int i = 0; i < method->argc; i++) {
            property_info(&arg_info[i], method->arg_types[i], &names[i], &classes[i], &hints[i],
                          method->arg_names[i]);
        }
        property_info(&ret_info, method->ret, &names[MAX_ARGS], &classes[MAX_ARGS],
                      &hints[MAX_ARGS], "");
        GDExtensionClassMethodInfo method_info;
        memset(&method_info, 0, sizeof method_info);
        method_info.name = &name;
        method_info.method_userdata = (void *)method;
        method_info.call_func = varcall;
        method_info.ptrcall_func = ptrcall;
        method_info.method_flags = GDEXTENSION_METHOD_FLAGS_DEFAULT;
        method_info.has_return_value = method->ret != T_NIL;
        method_info.return_value_info = &ret_info;
        method_info.argument_count = (uint32_t)method->argc;
        method_info.arguments_info = arg_info;
        method_info.arguments_metadata = metadata;
        api.register_method(api.library, &class_name, &method_info);
        for (int i = 0; i <= MAX_ARGS; i++) {
            if (i < method->argc || i == MAX_ARGS) api.destroy_string(&hints[i]);
        }
    }

    Value no_enum;
    name_of(&no_enum, "");
    for (size_t i = 0; i < sizeof constants / sizeof constants[0]; i++) {
        Value name;
        name_of(&name, constants[i].name);
        api.register_constant(api.library, &class_name, &no_enum, &name, constants[i].value, 0);
    }
}

static void initialize(void *userdata, GDExtensionInitializationLevel level) {
    (void)userdata;
    if (level == GDEXTENSION_INITIALIZATION_SCENE) register_everything();
}

static void deinitialize(void *userdata, GDExtensionInitializationLevel level) {
    (void)userdata;
    (void)level;
}

#define LOOKUP(field, type, name) \
    do { \
        api.field = (type)get_proc(name); \
        if (api.field == NULL) return 0; \
    } while (0)

__attribute__((visibility("default")))
GDExtensionBool rr2_library_init(GDExtensionInterfaceGetProcAddress get_proc,
                                 GDExtensionClassLibraryPtr library,
                                 GDExtensionInitialization *initialization) {
    api.library = library;
    LOOKUP(register_class, GDExtensionInterfaceClassdbRegisterExtensionClass6,
           "classdb_register_extension_class6");
    LOOKUP(register_method, GDExtensionInterfaceClassdbRegisterExtensionClassMethod,
           "classdb_register_extension_class_method");
    LOOKUP(register_constant, GDExtensionInterfaceClassdbRegisterExtensionClassIntegerConstant,
           "classdb_register_extension_class_integer_constant");
    LOOKUP(construct_object, GDExtensionInterfaceClassdbConstructObject3, "classdb_construct_object3");
    LOOKUP(method_ptrcall, GDExtensionInterfaceObjectMethodBindPtrcall, "object_method_bind_ptrcall");
    LOOKUP(set_instance, GDExtensionInterfaceObjectSetInstance, "object_set_instance");
    LOOKUP(string_name_new, GDExtensionInterfaceStringNameNewWithLatin1Chars,
           "string_name_new_with_latin1_chars");
    LOOKUP(string_new, GDExtensionInterfaceStringNewWithUtf8Chars, "string_new_with_utf8_chars");
    LOOKUP(string_to_utf8, GDExtensionInterfaceStringToUtf8Chars, "string_to_utf8_chars");
    LOOKUP(bytes_index, GDExtensionInterfacePackedByteArrayOperatorIndex,
           "packed_byte_array_operator_index");

    GDExtensionInterfaceVariantGetPtrConstructor get_constructor =
        (GDExtensionInterfaceVariantGetPtrConstructor)get_proc("variant_get_ptr_constructor");
    GDExtensionInterfaceVariantGetPtrDestructor get_destructor =
        (GDExtensionInterfaceVariantGetPtrDestructor)get_proc("variant_get_ptr_destructor");
    GDExtensionInterfaceVariantGetPtrBuiltinMethod get_builtin =
        (GDExtensionInterfaceVariantGetPtrBuiltinMethod)get_proc("variant_get_ptr_builtin_method");
    GDExtensionInterfaceGetVariantFromTypeConstructor get_to_variant =
        (GDExtensionInterfaceGetVariantFromTypeConstructor)get_proc(
            "get_variant_from_type_constructor");
    GDExtensionInterfaceGetVariantToTypeConstructor get_from_variant =
        (GDExtensionInterfaceGetVariantToTypeConstructor)get_proc(
            "get_variant_to_type_constructor");
    if (!get_constructor || !get_destructor || !get_builtin || !get_to_variant || !get_from_variant) {
        return 0;
    }
    api.bytes_new = get_constructor(T_BYTES, 0);
    api.destroy_string = get_destructor(T_STRING);
    api.destroy_string_name = get_destructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME);
    api.destroy_bytes = get_destructor(T_BYTES);
    static const GDExtensionVariantType used[] = {T_BOOL, T_INT, T_STRING, T_BYTES};
    for (size_t i = 0; i < sizeof used / sizeof used[0]; i++) {
        api.to_variant[used[i]] = get_to_variant(used[i]);
        api.from_variant[used[i]] = get_from_variant(used[i]);
    }
    GDExtensionInterfaceClassdbGetMethodBind get_method_bind =
        (GDExtensionInterfaceClassdbGetMethodBind)get_proc("classdb_get_method_bind");
    if (get_method_bind == NULL) return 0;
    Value object_class, notification;
    name_of(&object_class, "Object");
    name_of(&notification, "notification");
    api.object_notification = get_method_bind(&object_class, &notification, OBJECT_NOTIFICATION_HASH);
    if (api.object_notification == NULL) return 0;

    Value resize;
    name_of(&resize, "resize");
    api.bytes_resize = get_builtin(T_BYTES, &resize, PACKED_BYTE_ARRAY_RESIZE_HASH);
    if (!api.bytes_new || !api.bytes_resize || !api.destroy_string || !api.destroy_bytes) return 0;

    initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_SCENE;
    initialization->initialize = initialize;
    initialization->deinitialize = deinitialize;
    return 1;
}
