/*
 * VM Introspection
 *
 * Copyright (C) 2017-2020 Bitdefender S.R.L.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qom/object_interfaces.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "crypto/secret.h"
#include "crypto/hash.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"

#include "sysemu/vmi-handshake.h"

#define HANDSHAKE_TIMEOUT_SEC 10

typedef struct VMIntrospection {
    Object parent_obj;

    Error *init_error;

    char *chardevid;
    Chardev *chr;
    CharBackend sock;
    bool connected;

    char *keyid;
    Object *key;
    uint8_t cookie_hash[QEMU_VMI_COOKIE_HASH_SIZE];
    bool key_with_cookie;

    qemu_vmi_from_introspector hsk_in;
    uint64_t hsk_in_read_pos;
    uint64_t hsk_in_read_size;
    GSource *hsk_timer;
    uint32_t handshake_timeout;

    int64_t vm_start_time;

    Notifier machine_ready;
    bool created_from_command_line;

    bool kvmi_hooked;
} VMIntrospection;

#define TYPE_VM_INTROSPECTION "introspection"

#define VM_INTROSPECTION(obj) \
    OBJECT_CHECK(VMIntrospection, (obj), TYPE_VM_INTROSPECTION)

static Error *vm_introspection_init(VMIntrospection *i);

static void vmi_machine_ready(Notifier *notifier, void *data)
{
    VMIntrospection *i = container_of(notifier, VMIntrospection, machine_ready);

    i->init_error = vm_introspection_init(i);
    if (i->init_error) {
        Error *err = error_copy(i->init_error);

        error_report_err(err);
        if (i->created_from_command_line) {
            exit(1);
        }
    }
}

static void update_vm_start_time(VMIntrospection *i)
{
    i->vm_start_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) / 1000;
}

static void vmi_complete(UserCreatable *uc, Error **errp)
{
    VMIntrospection *i = VM_INTROSPECTION(uc);

    if (!i->chardevid) {
        error_setg(errp, "VMI: chardev is not set");
        return;
    }

    i->machine_ready.notify = vmi_machine_ready;
    qemu_add_machine_init_done_notifier(&i->machine_ready);

    /*
     * If the introspection object is created while parsing the command line,
     * the machine_ready callback will be called later. At that time,
     * it vm_introspection_init() fails, exit() will be called.
     *
     * If the introspection object is created through QMP, machine_init_done
     * is already set and qemu_add_machine_init_done_notifier() will
     * call our machine_done() callback. If vm_introspection_init() fails,
     * we don't call exit() and report the error back to the user.
     */
    if (i->init_error) {
        *errp = i->init_error;
        i->init_error = NULL;
        return;
    }

    update_vm_start_time(i);
}

static void prop_set_chardev(Object *obj, const char *value, Error **errp)
{
    VMIntrospection *i = VM_INTROSPECTION(obj);

    g_free(i->chardevid);
    i->chardevid = g_strdup(value);
}

static void prop_set_key(Object *obj, const char *value, Error **errp)
{
    VMIntrospection *i = VM_INTROSPECTION(obj);

    g_free(i->keyid);
    i->keyid = g_strdup(value);
}

static void prop_get_uint32(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    uint32_t *value = opaque;

    visit_type_uint32(v, name, value, errp);
}

static void prop_set_uint32(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    uint32_t *value = opaque;
    Error *local_err = NULL;

    visit_type_uint32(v, name, value, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
    }
}

static void class_init(ObjectClass *oc, void *data)
{
    UserCreatableClass *uc = USER_CREATABLE_CLASS(oc);

    uc->complete = vmi_complete;
}

static void instance_init(Object *obj)
{
    VMIntrospection *i = VM_INTROSPECTION(obj);

    i->created_from_command_line = (qdev_hotplug == false);

    object_property_add_str(obj, "chardev", NULL, prop_set_chardev, NULL);
    object_property_add_str(obj, "key", NULL, prop_set_key, NULL);

    i->handshake_timeout = HANDSHAKE_TIMEOUT_SEC;
    object_property_add(obj, "handshake_timeout", "uint32",
                        prop_set_uint32, prop_get_uint32,
                        NULL, &i->handshake_timeout, NULL);
}

static void disconnect_chardev(VMIntrospection *i)
{
    if (i->connected) {
        qemu_chr_fe_disconnect(&i->sock);
    }
}

static void unhook_kvmi(VMIntrospection *i)
{
    if (i->kvmi_hooked) {
        if (kvm_vm_ioctl(kvm_state, KVM_INTROSPECTION_UNHOOK, NULL)) {
            error_report("VMI: ioctl/KVM_INTROSPECTION_UNHOOK failed, errno %d",
                         errno);
        }
        i->kvmi_hooked = false;
    }
}

static void disconnect_and_unhook_kvmi(VMIntrospection *i)
{
    disconnect_chardev(i);
    unhook_kvmi(i);
}

static void cancel_timer(GSource *timer)
{
    if (timer) {
        g_source_destroy(timer);
        g_source_unref(timer);
    }
}

static void cancel_handshake_timer(VMIntrospection *i)
{
    cancel_timer(i->hsk_timer);
    i->hsk_timer = NULL;
}

static void instance_finalize(Object *obj)
{
    VMIntrospection *i = VM_INTROSPECTION(obj);

    g_free(i->chardevid);
    g_free(i->keyid);

    cancel_handshake_timer(i);

    if (i->chr) {
        qemu_chr_fe_deinit(&i->sock, true);
    }

    error_free(i->init_error);
}

static const TypeInfo info = {
    .name              = TYPE_VM_INTROSPECTION,
    .parent            = TYPE_OBJECT,
    .class_init        = class_init,
    .instance_size     = sizeof(VMIntrospection),
    .instance_finalize = instance_finalize,
    .instance_init     = instance_init,
    .interfaces        = (InterfaceInfo[]){
        {TYPE_USER_CREATABLE},
        {}
    }
};

static void register_types(void)
{
    type_register_static(&info);
}

type_init(register_types);

static bool send_handshake_info(VMIntrospection *i, Error **errp)
{
    qemu_vmi_to_introspector send = {};
    const char *vm_name;
    int r;

    send.struct_size = sizeof(send);
    send.start_time = i->vm_start_time;
    memcpy(&send.uuid, &qemu_uuid, sizeof(send.uuid));
    vm_name = qemu_get_vm_name();
    if (vm_name) {
        snprintf(send.name, sizeof(send.name), "%s", vm_name);
        send.name[sizeof(send.name) - 1] = 0;
    }

    r = qemu_chr_fe_write_all(&i->sock, (uint8_t *)&send, sizeof(send));
    if (r != sizeof(send)) {
        error_setg_errno(errp, errno, "VMI: error writing to '%s'",
                         i->chardevid);
        return false;
    }

    /* tcp_chr_write may call tcp_chr_disconnect/CHR_EVENT_CLOSED */
    if (!i->connected) {
        error_setg(errp, "VMI: qemu_chr_fe_write_all() failed");
        return false;
    }

    return true;
}

static bool validate_handshake_cookie(VMIntrospection *i)
{
    if (!i->key_with_cookie) {
        return true;
    }

    return 0 == memcmp(&i->cookie_hash, &i->hsk_in.cookie_hash,
                       sizeof(i->cookie_hash));
}

static bool validate_handshake(VMIntrospection *i, Error **errp)
{
    uint32_t min_accepted_size;

    min_accepted_size = offsetof(qemu_vmi_from_introspector, cookie_hash)
                        + QEMU_VMI_COOKIE_HASH_SIZE;

    if (i->hsk_in.struct_size < min_accepted_size) {
        error_setg(errp, "VMI: not enough or invalid handshake data");
        return false;
    }

    if (!validate_handshake_cookie(i)) {
        error_setg(errp, "VMI: received cookie doesn't match");
        return false;
    }

    /*
     * Check hsk_in.struct_size and sizeof(hsk_in) before accessing any
     * other fields. We might get fewer bytes from applications using
     * old versions if we extended the qemu_vmi_from_introspector structure.
     */

    return true;
}

static bool connect_kernel(VMIntrospection *i, Error **errp)
{
    struct kvm_introspection_feature commands, events;
    struct kvm_introspection_hook kernel;
    const __s32 all_ids = -1;

    memset(&kernel, 0, sizeof(kernel));
    memcpy(kernel.uuid, &qemu_uuid, sizeof(kernel.uuid));
    kernel.fd = object_property_get_int(OBJECT(i->chr), "fd", NULL);

    if (kvm_vm_ioctl(kvm_state, KVM_INTROSPECTION_HOOK, &kernel)) {
        error_setg_errno(errp, errno,
                         "VMI: ioctl/KVM_INTROSPECTION_HOOK failed");
        if (errno == EPERM) {
            error_append_hint(errp,
                              "Reload the kvm module with kvm.introspection=on\n");
        }

        return false;
    }

    commands.allow = 1;
    commands.id = all_ids;
    if (kvm_vm_ioctl(kvm_state, KVM_INTROSPECTION_COMMAND, &commands)) {
        error_setg_errno(errp, errno,
                         "VMI: ioctl/KVM_INTROSPECTION_COMMAND failed");
        goto error;
    }

    events.allow = 1;
    events.id = all_ids;
    if (kvm_vm_ioctl(kvm_state, KVM_INTROSPECTION_EVENT, &events)) {
        error_setg_errno(errp, errno,
                         "VMI: ioctl/KVM_INTROSPECTION_EVENT failed");
        goto error;
    }

    info_report("VMI: machine hooked");
    i->kvmi_hooked = true;

    return true;

error:
    if (kvm_vm_ioctl(kvm_state, KVM_INTROSPECTION_UNHOOK, NULL)) {
        error_setg_errno(errp, errno,
                         "VMI: ioctl/KVM_INTROSPECTION_UNHOOK failed");
    }

    return false;
}

/*
 * We should read only the handshake structure,
 * which might have a different size than what we expect.
 */
static int vmi_chr_can_read(void *opaque)
{
    VMIntrospection *i = opaque;

    if (i->hsk_timer == NULL || !i->connected) {
        return 0;
    }

    /* first, we read the incoming structure size */
    if (i->hsk_in_read_pos == 0) {
        return sizeof(i->hsk_in.struct_size);
    }

    /* validate the incoming structure size */
    if (i->hsk_in.struct_size < sizeof(i->hsk_in.struct_size)) {
        return 0;
    }

    /* read the rest of the incoming structure */
    return i->hsk_in.struct_size - i->hsk_in_read_pos;
}

static bool enough_bytes_for_handshake(VMIntrospection *i)
{
    return i->hsk_in_read_pos  >= sizeof(i->hsk_in.struct_size)
        && i->hsk_in_read_size == i->hsk_in.struct_size;
}

static bool validate_and_connect(VMIntrospection *i, Error **errp)
{
    if (!validate_handshake(i, errp)) {
        return false;
    }

    if (!connect_kernel(i, errp)) {
        return false;
    }

    return true;
}

static void vmi_chr_read(void *opaque, const uint8_t *buf, int size)
{
    VMIntrospection *i = opaque;
    size_t to_read;

    i->hsk_in_read_size += size;

    to_read = sizeof(i->hsk_in) - i->hsk_in_read_pos;
    if (to_read > size) {
        to_read = size;
    }

    if (to_read) {
        memcpy((uint8_t *)&i->hsk_in + i->hsk_in_read_pos, buf, to_read);
        i->hsk_in_read_pos += to_read;
    }

    if (enough_bytes_for_handshake(i)) {
        Error *local_err = NULL;

        cancel_handshake_timer(i);

        if (!validate_and_connect(i, &local_err)) {
            error_append_hint(&local_err, "reconnecting\n");
            warn_report_err(local_err);
            qemu_chr_fe_disconnect(&i->sock);
        }
    }
}

static gboolean vmi_hsk_timeout(gpointer opaque)
{
    VMIntrospection *i = opaque;

    warn_report("VMI: the handshake takes too long");
    disconnect_and_unhook_kvmi(i);

    g_source_unref(i->hsk_timer);
    i->hsk_timer = NULL;
    return G_SOURCE_REMOVE;
}

static void vmi_start_handshake(void *opaque)
{
    VMIntrospection *i = opaque;
    Error *local_err = NULL;

    if (!send_handshake_info(i, &local_err)) {
        error_append_hint(&local_err, "reconnecting\n");
        warn_report_err(local_err);
        qemu_chr_fe_disconnect(&i->sock);
        return;
    }

    info_report("VMI: handshake started");
}

static void vmi_chr_event_open(VMIntrospection *i)
{
    i->connected = true;

    memset(&i->hsk_in, 0, sizeof(i->hsk_in));
    i->hsk_in_read_pos = 0;
    i->hsk_in_read_size = 0;

    i->hsk_timer = qemu_chr_timeout_add_ms(i->chr,
                                           i->handshake_timeout * 1000,
                                           vmi_hsk_timeout, i);

    vmi_start_handshake(i);
}

static void vmi_chr_event_closed(VMIntrospection *i)
{
    i->connected = false;

    if (i->kvmi_hooked) {
        warn_report("VMI: introspection tool disconnected");
        disconnect_and_unhook_kvmi(i);
    }

    cancel_handshake_timer(i);
}

static void vmi_chr_event(void *opaque, int event)
{
    VMIntrospection *i = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        vmi_chr_event_open(i);
        break;
    case CHR_EVENT_CLOSED:
        vmi_chr_event_closed(i);
        break;
    default:
        break;
    }
}

static bool make_cookie_hash(const char *key_id, uint8_t *cookie_hash,
                             Error **errp)
{
    uint8_t *cookie = NULL, *hash = NULL;
    size_t cookie_size, hash_size = 0;
    bool done = false;

    if (qcrypto_secret_lookup(key_id, &cookie, &cookie_size, errp) == 0
            && qcrypto_hash_bytes(QCRYPTO_HASH_ALG_SHA1,
                                  (const char *)cookie, cookie_size,
                                  &hash, &hash_size, errp) == 0) {
        if (hash_size == QEMU_VMI_COOKIE_HASH_SIZE) {
            memcpy(cookie_hash, hash, QEMU_VMI_COOKIE_HASH_SIZE);
            done = true;
        } else {
            error_setg(errp, "VMI: hash algorithm size mismatch");
        }
    }

    g_free(cookie);
    g_free(hash);

    return done;
}

static Error *vm_introspection_init(VMIntrospection *i)
{
    Error *err = NULL;
    int kvmi_version;
    Chardev *chr;

    if (!kvm_enabled()) {
        error_setg(&err, "VMI: missing KVM support");
        return err;
    }

    kvmi_version = kvm_check_extension(kvm_state, KVM_CAP_INTROSPECTION);
    if (kvmi_version == 0) {
        error_setg(&err,
                   "VMI: missing kernel built with CONFIG_KVM_INTROSPECTION");
        return err;
    }

    if (i->keyid) {
        if (!make_cookie_hash(i->keyid, i->cookie_hash, &err)) {
            return err;
        }
        i->key_with_cookie = true;
    } else {
        warn_report("VMI: the introspection tool won't be 'authenticated'");
    }

    chr = qemu_chr_find(i->chardevid);
    if (!chr) {
        error_setg(&err, "VMI: device '%s' not found", i->chardevid);
        return err;
    }

    if (!qemu_chr_fe_init(&i->sock, chr, &err)) {
        error_append_hint(&err, "VMI: device '%s' not initialized\n",
                          i->chardevid);
        return err;
    }

    i->chr = chr;

    if (qemu_chr_fe_reconnect_time(&i->sock, -1) <= 0) {
        error_setg(&err, "VMI: missing reconnect=N for '%s'",
                          i->chardevid);
        return err;
    }

    qemu_chr_fe_set_handlers(&i->sock, vmi_chr_can_read, vmi_chr_read,
                             vmi_chr_event, NULL, i, NULL, true);

    /*
     * The reconnect timer is triggered by either machine init or by a chardev
     * disconnect. For the QMP creation, when the machine is already started,
     * use an artificial disconnect just to restart the timer.
     */
    if (!i->created_from_command_line) {
        qemu_chr_fe_disconnect(&i->sock);
    }

    return NULL;
}
