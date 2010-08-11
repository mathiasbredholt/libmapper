
#include <lo/lo.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "mapper_internal.h"
#include "types_internal.h"
#include "config.h"
#include <mapper/mapper.h>

/*! Internal function to get the current time. */
static double get_current_time()
{
#ifdef HAVE_GETTIMEOFDAY
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec + tv.tv_usec / 1000000.0;
#else
#error No timing method known on this platform.
#endif
}

/* Internal message handler prototypes. */
static int handler_who(const char *, const char *, lo_arg **, int,
                       lo_message, void *);
static int handler_registered(const char *, const char *, lo_arg **,
                              int, lo_message, void *);
static int handler_logout(const char *, const char *, lo_arg **,
                          int, lo_message, void *);
static int handler_id_n_namespace_input_get(const char *, const char *,
                                            lo_arg **, int, lo_message,
                                            void *);
static int handler_id_n_namespace_output_get(const char *, const char *,
                                             lo_arg **, int, lo_message,
                                             void *);
static int handler_id_n_namespace_get(const char *, const char *,
                                      lo_arg **, int, lo_message, void *);
static int handler_device_alloc_port(const char *, const char *, lo_arg **,
                                     int, lo_message, void *);
static int handler_device_alloc_name(const char *, const char *, lo_arg **,
                                     int, lo_message, void *);
static int handler_device_link(const char *, const char *, lo_arg **, int,
                               lo_message, void *);
static int handler_device_link_to(const char *, const char *, lo_arg **,
                                  int, lo_message, void *);
static int handler_device_linked(const char *, const char *, lo_arg **,
                                 int, lo_message, void *);
static int handler_device_unlink(const char *, const char *, lo_arg **,
                                 int, lo_message, void *);
static int handler_device_unlinked(const char *, const char *, lo_arg **,
                                   int, lo_message, void *);
static int handler_device_links_get(const char *, const char *, lo_arg **,
                                    int, lo_message, void *);
static int handler_param_connect(const char *, const char *, lo_arg **,
                                 int, lo_message, void *);
static int handler_param_connect_to(const char *, const char *, lo_arg **,
                                    int, lo_message, void *);
static int handler_param_connected(const char *, const char *, lo_arg **,
                                   int, lo_message, void *);
static int handler_param_connection_modify(const char *, const char *,
                                           lo_arg **, int, lo_message,
                                           void *);
static int handler_param_disconnect(const char *, const char *, lo_arg **,
                                    int, lo_message, void *);
static int handler_param_disconnected(const char *, const char *, lo_arg **,
                                      int, lo_message, void *);
static int handler_device_connections_get(const char *, const char *,
                                          lo_arg **, int, lo_message,
                                          void *);

/* Handler <-> Message relationships */
static struct { char* path; char *types; lo_method_handler h; }
handlers[] = {
    {"/who",                    "",         handler_who},
    {"/registered",             NULL,       handler_registered},
    {"/logout",                 NULL,       handler_logout},
    {"%s/namespace/get",        "",         handler_id_n_namespace_get},
    {"%s/namespace/input/get",  "",         handler_id_n_namespace_input_get},
    {"%s/namespace/output/get", "",         handler_id_n_namespace_output_get},
    {"%s/info/get",             "",         handler_who},
    {"%s/links/get",            "",         handler_device_links_get},
    {"/link",                   "ss",       handler_device_link},
    {"/link_to",                "sssssiss", handler_device_link_to},
    {"/linked",                 "ss",       handler_device_linked},
    {"/unlink",                 "ss",       handler_device_unlink},
    {"/unlinked",               "ss",       handler_device_unlinked},
    {"%s/connections/get",      "",         handler_device_connections_get},
    {"/connect",                NULL,       handler_param_connect},
    {"/connect_to",             NULL,       handler_param_connect_to},
    {"/connected",              NULL,       handler_param_connected},
    {"/connection/modify",      NULL,       handler_param_connection_modify},
    {"/disconnect",             "ss",       handler_param_disconnect},
    {"/disconnected",           "ss",       handler_param_disconnected},
};
const int N_HANDLERS = sizeof(handlers)/sizeof(handlers[0]);

/* Internal LibLo error handler */
static void handler_error(int num, const char *msg, const char *where)
{
    printf("[libmapper] liblo server error %d in path %s: %s\n",
           num, where, msg);
}

/* Functions for handling the resource allocation scheme.  If
 * check_collisions() returns 1, the resource in question should be
 * probed on the admin bus. */
static int check_collisions(mapper_admin admin,
                            mapper_admin_allocated_t *resource);
static void on_collision(mapper_admin_allocated_t *resource,
                         mapper_admin admin, int type);

/*! Local function to get the IP address of a network interface. */
static struct in_addr get_interface_addr(const char *ifname)
{
    struct ifaddrs *ifaphead;
    struct ifaddrs *ifap;
    struct in_addr error = { 0 };

    if (getifaddrs(&ifaphead) != 0)
        return error;

    ifap = ifaphead;
    while (ifap) {
        struct sockaddr_in *sa = (struct sockaddr_in *) ifap->ifa_addr;
        if (!sa) {
            trace("ifap->ifa_addr = 0, unknown condition.\n");
            ifap = ifap->ifa_next;
            continue;
        }
        if (sa->sin_family == AF_INET
            && strcmp(ifap->ifa_name, ifname) == 0) {
            return sa->sin_addr;
        }
        ifap = ifap->ifa_next;
    }

    return error;
}

/*! Allocate and initialize a new admin structure.
 *  \param identifier An identifier for this device which does not
 *  need to be unique.
 *  \param type The device type for this device. (Data direction,
 *  functionality.)
 *  \param initial_port The initial UDP port to use for this
 *  device. This will likely change within a few minutes after the
 *  device is allocated.
 *  \return A newly initialized mapper admin structure.
 */
mapper_admin mapper_admin_new(const char *identifier,
                              mapper_device device, int initial_port)
{
    mapper_admin admin = (mapper_admin)malloc(sizeof(mapper_admin_t));
    if (!admin)
        return NULL;

    /* Initialize interface information.  We'll use defaults for now,
     * perhaps this should be configurable in the future. */
    {
        char *eths[] = { "eth0", "eth1", "eth2", "eth3", "eth4",
                         "en0", "en1", "en2", "en3", "en4", "lo" };
        int num = sizeof(eths) / sizeof(char *), i;
        for (i = 0; i < num; i++) {
            admin->interface_ip = get_interface_addr(eths[i]);
            if (admin->interface_ip.s_addr != 0) {
                strcpy(admin->interface, eths[i]);
                break;
            }
        }
        if (i >= num) {
            trace("no interface found\n");
        }
    }

    /* Open address for multicast group 224.0.1.3, port 7570 */
    admin->admin_addr = lo_address_new("224.0.1.3", "7570");
    if (!admin->admin_addr) {
        free(admin->identifier);
        free(admin);
        return NULL;
    }

    /* Set TTL for packet to 1 -> local subnet */
    lo_address_set_ttl(admin->admin_addr, 1);

    /* Open server for multicast group 224.0.1.3, port 7570 */
    admin->admin_server =
        lo_server_new_multicast("224.0.1.3", "7570", handler_error);
    if (!admin->admin_server) {
        free(admin->identifier);
        lo_address_free(admin->admin_addr);
        free(admin);
        return NULL;
    }

    /* Initialize data structures */
    admin->identifier = strdup(identifier);
    admin->name = 0;
    admin->ordinal.value = 1;
    admin->ordinal.locked = 0;
    admin->ordinal.collision_count = -1;
    admin->ordinal.count_time = get_current_time();
    admin->ordinal.on_collision = mapper_admin_name_registered;
    admin->port.value = initial_port;
    admin->port.locked = 0;
    admin->port.collision_count = -1;
    admin->port.count_time = get_current_time();
    admin->port.on_collision = mapper_admin_port_registered;
    admin->registered = 0;
    admin->device = device;

    /* Add methods for admin bus.  Only add methods needed for
     * allocation here. Further methods are added when the device is
     * registered. */
    lo_server_add_method(admin->admin_server, "/port/probe", NULL,
                         handler_device_alloc_port, admin);
    lo_server_add_method(admin->admin_server, "/name/probe", NULL,
                         handler_device_alloc_name, admin);
    lo_server_add_method(admin->admin_server, "/port/registered", NULL,
                         handler_device_alloc_port, admin);
    lo_server_add_method(admin->admin_server, "/name/registered", NULL,
                         handler_device_alloc_name, admin);

    /* Resource allocation algorithm needs a seeded random number
     * generator. */
    srand(((unsigned int)(get_current_time()*1000000.0))%100000);

    /* Probe potential port and name to admin bus. */
    mapper_admin_port_probe(admin);
    mapper_admin_name_probe(admin);

    return admin;
}

/*! Free the memory allocated by a mapper admin structure.
 *  \param admin An admin structure handle.
 */
void mapper_admin_free(mapper_admin admin)
{
    if (!admin)
        return;

    if (admin->identifier)
        free(admin->identifier);

    if (admin->name)
        free(admin->name);

    if (admin->admin_server)
        lo_server_free(admin->admin_server);

    if (admin->admin_addr)
        lo_address_free(admin->admin_addr);

    free(admin);
}

/*! This is the main function to be called once in a while from a
 *  program so that the admin bus can be automatically managed.
 */
void mapper_admin_poll(mapper_admin admin)
{

    int count = 0;

    while (count < 10 && lo_server_recv_noblock(admin->admin_server, 0)) {
        count++;
    }


    /* If the port is not yet locked, process collision timing.  Once
     * the port is locked it won't change. */
    if (!admin->port.locked)
        if (check_collisions(admin, &admin->port))
            /* If the port has changed, re-probe the new potential port. */
            mapper_admin_port_probe(admin);

    /* If the ordinal is not yet locked, process collision timing.
     * Once the ordinal is locked it won't change. */
    if (!admin->ordinal.locked)
        if (check_collisions(admin, &admin->ordinal))
            /* If the ordinal has changed, re-probe the new name. */
            mapper_admin_name_probe(admin);

    /* If we are ready to register the device, add the needed message
     * handlers. */
    if (!admin->registered && admin->port.locked && admin->ordinal.locked) {
        int i;
        for (i=0; i < N_HANDLERS; i++)
        {
            char fullpath[256];
            snprintf(fullpath, 256, handlers[i].path,
                     mapper_admin_name(admin));
            lo_server_add_method(admin->admin_server, fullpath,
                                 handlers[i].types, handlers[i].h,
                                 admin);
        }

        /* Remove some handlers needed during allocation. */
        lo_server_del_method(admin->admin_server, "/port/registered", NULL);
        lo_server_del_method(admin->admin_server, "/name/registered", NULL);

        admin->registered = 1;
        trace("</%s.?::%p> registered as <%s>\n",
              admin->identifier, admin, mapper_admin_name(admin));
        mapper_admin_send_osc(admin, "/who", "");
    }
}

/*! Probe the admin bus to see if a device's proposed port is already
 *  taken.
 */
void mapper_admin_port_probe(mapper_admin admin)
{
    trace("</%s.?::%p> probing port\n", admin->identifier, admin);

    /* We don't use mapper_admin_send_osc() here because the name is
     * not yet established and it would trigger a warning. */
    lo_send(admin->admin_addr, "/port/probe", "i", admin->port.value);
}

/*! Probe the admin bus to see if a device's proposed name.ordinal is
 *  already taken.
 */
void mapper_admin_name_probe(mapper_admin admin)
{
    /* Note: mapper_admin_name() would refuse here since the
     * ordinal is not yet locked, so we have to build it manually at
     * this point. */
    char name[256];
    trace("</%s.?::%p> probing name\n", admin->identifier, admin);
    snprintf(name, 256, "/%s.%d", admin->identifier, admin->ordinal.value);

    /* For the same reason, we can't use mapper_admin_send_osc()
     * here. */
    lo_send(admin->admin_addr, "/name/probe", "s", name);
}

/*! Announce on the admin bus a device's registered port. */
void mapper_admin_port_registered(mapper_admin admin)
{
    if (admin->port.locked)
        /* Name not yet registered, so we can't use
         * mapper_admin_send_osc() here. */
        lo_send(admin->admin_addr, "/port/registered",
                "i", admin->port.value);
}

/*! Announce on the admin bus a device's registered name.ordinal. */
void mapper_admin_name_registered(mapper_admin admin)
{
    if (admin->ordinal.locked)
        mapper_admin_send_osc(admin, "/name/registered",
                              "s", mapper_admin_name(admin));
}

const char *_real_mapper_admin_name(mapper_admin admin,
                                    const char *file, unsigned int line)
{
    if (!admin->ordinal.locked) {
        /* Since this function is intended to be used internally in a
         * fairly liberal manner, we want to trace any situations
         * where returning 0 might cause a problem.  The external call
         * to this function, mdev_full_name(), has been special-cased
         * to allow this. */
        trace("mapper_admin_name() returning 0 at %s:%d.\n", file, line);
        return 0;
    }

    if (admin->name)
        return admin->name;

    unsigned int len = strlen(admin->identifier) + 6;
    admin->name = (char *) malloc(len);
    admin->name[0] = 0;
    snprintf(admin->name, len, "/%s.%d", admin->identifier,
             admin->ordinal.value);

    return admin->name;
}

/*! Algorithm for checking collisions and allocating resources. */
static int check_collisions(mapper_admin admin,
                            mapper_admin_allocated_t *resource)
{
    double timediff;

    if (resource->locked)
        return 0;

    timediff = get_current_time() - resource->count_time;

    if (timediff >= 2.0) {
        resource->locked = 1;
        if (resource->on_lock)
            resource->on_lock(admin->device, resource);
    }

    else
        /* If port collisions were found within 500 milliseconds of the
         * last probe, try a new random port. */
    if (timediff >= 0.5 && resource->collision_count > 0) {
        /* Otherwise, add a random number based on the number of
         * collisions. */
        resource->value += rand() % (resource->collision_count + 1);

        /* Prepare for causing new port collisions. */

        resource->collision_count = -1;
        resource->count_time = get_current_time();

        /* Indicate that we need to re-probe the new value. */
        return 1;
    }

    return 0;
}

static void on_collision(mapper_admin_allocated_t *resource,
                         mapper_admin admin, int type)
{
    if (resource->locked && resource->on_collision)
        resource->on_collision(admin);

    /* Count port collisions. */
    resource->collision_count++;
    trace("%d collision_count = %d\n", resource->value,
          resource->collision_count);
    resource->count_time = get_current_time();
}

void _real_mapper_admin_send_osc(mapper_admin admin, const char *path,
                                 const char *types, ...)
{
    char namedpath[1024];
    snprintf(namedpath, 1024, path, mapper_admin_name(admin));

    char t[]=" ";

    lo_message m = lo_message_new();
    if (!m) {
        trace("couldn't allocate lo_message\n");
        return;
    }

    va_list aq;
    va_start(aq, types);

    while (types && *types) {
        t[0] = types[0];
        switch (t[0]) {
        case 'i': lo_message_add(m, t, va_arg(aq, int)); break;
        case 's': lo_message_add(m, t, va_arg(aq, char*)); break;
        case 'f': lo_message_add(m, t, va_arg(aq, double)); break;
        default:
            die_unless(0, "message %s, unknown type '%c'\n",
                       path, t[0]);
        }
        types++;
    }

    mapper_msg_prepare_varargs(m, aq);

    va_end(aq);

    lo_send_message(admin->admin_addr, namedpath, m);
    lo_message_free(m);
}

void mapper_admin_send_osc_with_params(mapper_admin admin,
                                       mapper_message_t *params,
                                       const char *path,
                                       const char *types, ...)
{
    char namedpath[1024];
    snprintf(namedpath, 1024, path, mapper_admin_name(admin));

    lo_message m = lo_message_new();
    if (!m) {
        trace("couldn't allocate lo_message\n");
        return;
    }

    va_list aq;
    va_start(aq, types);
    lo_message_add_varargs(m, types, aq);

    mapper_msg_prepare_params(m, params);

    lo_send_message(admin->admin_addr, namedpath, m);
    lo_message_free(m);
}

/**********************************/
/* Internal OSC message handlers. */
/**********************************/

/*! Respond to /who by announcing the current port. */
static int handler_who(const char *path, const char *types, lo_arg **argv,
                       int argc, lo_message msg, void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;

    mapper_admin_send_osc(
        admin, "/registered", "s", mapper_admin_name(admin),
        AT_IP, inet_ntoa(admin->interface_ip),
        AT_PORT, admin->port.value,
        AT_CANALIAS, 0,
        AT_NUMINPUTS, admin->device ? mdev_num_inputs(admin->device) : 0,
        AT_NUMOUTPUTS, admin->device ? mdev_num_outputs(admin->device) : 0,
        AT_HASH, 0);

    return 0;
}


/*! Register information about port and host for the device. */
static int handler_registered(const char *path, const char *types,
                              lo_arg **argv, int argc, lo_message msg,
                              void *user_data)
{
    if (argc < 1)
        return 0;

    if (types[0] != 's' && types[0] != 'S')
        return 0;

    const char *name = &argv[0]->s;

    mapper_message_t params;
    mapper_msg_parse_params(&params, path, &types[1],
                            argc-1, &argv[1]);

    mapper_db_add_or_update_params(name, &params);

    return 0;
}

/*! Respond to /logout by deleting record of device. */
static int handler_logout(const char *path, const char *types,
                          lo_arg **argv, int argc, lo_message msg,
                          void *user_data)
{
    mapper_admin admin = (mapper_admin)user_data;

    if (argc < 1)
        return 0;

    if (types[0] != 's' && types[0] != 'S')
        return 0;

    const char *name = &argv[0]->s;

    //TO DO: remove record of device from database
    trace("<%s> got /logout %s\n", mapper_admin_name(admin), name);

    return 0;
}

/*! Respond to /namespace/input/get by enumerating all supported
 *  inputs. */
static int handler_id_n_namespace_input_get(const char *path,
                                            const char *types,
                                            lo_arg **argv, int argc,
                                            lo_message msg,
                                            void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;
    mapper_device md = admin->device;
    int i;

    for (i = 0; i < md->n_inputs; i++) {
        mapper_signal sig = md->inputs[i];
        mapper_admin_send_osc(
            admin, "%s/namespace/input", "s", sig->name,
            AT_TYPE, sig->type,
            sig->minimum ? AT_MIN : -1, sig,
            sig->maximum ? AT_MAX : -1, sig);
    }

    return 0;
}

/*! Respond to /namespace/output/get by enumerating all supported
 *  outputs. */
static int handler_id_n_namespace_output_get(const char *path,
                                             const char *types,
                                             lo_arg **argv, int argc,
                                             lo_message msg,
                                             void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;
    mapper_device md = admin->device;
    int i;

    for (i = 0; i < md->n_outputs; i++) {
        mapper_signal sig = md->outputs[i];
        mapper_admin_send_osc(
            admin, "%s/namespace/output", "s", sig->name,
            AT_TYPE, sig->type,
            sig->minimum ? AT_MIN : -1, sig,
            sig->maximum ? AT_MAX : -1, sig);
    }

    return 0;
}

/*! Respond to /namespace/get by enumerating all supported inputs and
 *  outputs. */
static int handler_id_n_namespace_get(const char *path, const char *types,
                                      lo_arg **argv, int argc,
                                      lo_message msg, void *user_data)
{

    handler_id_n_namespace_input_get(path, types, argv, argc, msg,
                                     user_data);
    handler_id_n_namespace_output_get(path, types, argv, argc, msg,
                                      user_data);

    return 0;

}

static int handler_device_alloc_port(const char *path, const char *types,
                                     lo_arg **argv, int argc,
                                     lo_message msg, void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;


    unsigned int probed_port = 0;

    if (argc < 1)
        return 0;

    if (types[0] == 'i')
        probed_port = argv[0]->i;
    else if (types[0] == 'f')
        probed_port = (unsigned int) argv[0]->f;
    else
        return 0;

    trace("</%s.?::%p> got /port/probe %d \n",
          admin->identifier, admin, probed_port);

    /* Process port collisions. */
    if (probed_port == admin->port.value)
        on_collision(&admin->port, admin, 0);

    return 0;
}

static int handler_device_alloc_name(const char *path, const char *types,
                                     lo_arg **argv, int argc,
                                     lo_message msg, void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;


    char *probed_name = 0, *s;
    unsigned int probed_ordinal = 0;

    if (argc < 1)
        return 0;

    if (types[0] != 's' && types[0] != 'S')
        return 0;

    probed_name = &argv[0]->s;

    /* Parse the ordinal from the complete name which is in the
     * format: /<name>.<n> */
    s = probed_name;
    if (*s++ != '/')
        return 0;
    while (*s != '.' && *s++) {
    }
    probed_ordinal = atoi(++s);

    trace("</%s.?::%p> got /name/probe %s\n",
          admin->identifier, admin, probed_name);

    /* Process ordinal collisions. */
    //The collision should be calculated separately per-device-name
    strtok(probed_name, ".");
    probed_name++;
    if ((strcmp(probed_name, admin->identifier) == 0)
        && (probed_ordinal == admin->ordinal.value))
        on_collision(&admin->ordinal, admin, 1);

    return 0;
}

/*! Link two devices. */
static int handler_device_link(const char *path, const char *types,
                               lo_arg **argv, int argc, lo_message msg,
                               void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;
    const char *sender_name, *target_name;

    if (argc < 2)
        return 0;

    if (types[0] != 's' && types[0] != 'S' && types[1] != 's'
        && types[1] != 'S')
        return 0;

    sender_name = &argv[0]->s;
    target_name = &argv[1]->s;

    trace("<%s> got /link %s %s\n", mapper_admin_name(admin),
          sender_name, target_name);

    /* If the device who received the message is the target in the
     * /link message... */
    if (strcmp(mapper_admin_name(admin), target_name) == 0) {
        mapper_admin_send_osc(
            admin, "/link_to", "ss", sender_name, target_name,
            AT_IP, inet_ntoa(admin->interface_ip),
            AT_PORT, admin->port.value,
            AT_CANALIAS, 0);
    }
    return 0;
}

/*! Link two devices... continued. */
static int handler_device_link_to(const char *path, const char *types,
                                  lo_arg **argv, int argc, lo_message msg,
                                  void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;
    mapper_device md = admin->device;
    mapper_router router = md->routers;

    const char *sender_name, *target_name, *host=0, *canAlias=0;
    int port;
    mapper_message_t params;

    if (argc < 2)
        return 0;

    if (types[0] != 's' && types[0] != 'S' && types[1] != 's'
        && types[1] != 'S')
        return 0;

    sender_name = &argv[0]->s;
    target_name = &argv[1]->s;

    if (strcmp(sender_name, mapper_admin_name(admin)))
    {
        trace("<%s> ignoring /link_to %s %s\n",
              mapper_admin_name(admin), sender_name, target_name);
        return 0;
    }

    trace("<%s> got /link_to %s %s\n", mapper_admin_name(admin),
          sender_name, target_name);

    // Discover whether the device is already linked.
    while (router) {
        if (strcmp(router->target_name, target_name)==0)
            break;
        router = router->next;
    }

    if (router)
        // Already linked, nothing to do.
        return 0;

    // Parse the message.
    if (mapper_msg_parse_params(&params, path, &types[2],
                                argc-2, &argv[2]))
        return 0;

    // Check the results.
    host = mapper_msg_get_param_if_string(&params, AT_IP);
    if (!host) {
        trace("can't perform /link_to, host unknown\n");
        return 0;
    }

    if (mapper_msg_get_param_if_int(&params, AT_PORT, &port)) {
        trace("can't perform /link_to, port unknown\n");
        return 0;
    }

    canAlias = mapper_msg_get_param_if_string(&params, AT_CANALIAS);

    // Creation of a new router added to the sender.
    router = mapper_router_new(md, host, port, target_name);
    mdev_add_router(md, router);
    md->num_routers ++;

    // Announce the result.
    mapper_admin_send_osc(admin, "/linked", "ss",
                          mapper_admin_name(admin), target_name);

    trace("new router to %s -> host: %s, port: %d, canAlias: %s\n",
          target_name, host, port, canAlias ? canAlias : "no");

    return 0;
}

/*! Store record of linked devices. */
static int handler_device_linked(const char *path, const char *types,
                                 lo_arg **argv, int argc, lo_message msg,
                                 void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;
    const char *sender_name, *target_name;

    if (argc < 2)
        return 0;

    if (types[0] != 's' && types[0] != 'S' && types[1] != 's'
        && types[1] != 'S')
        return 0;

    sender_name = &argv[0]->s;
    target_name = &argv[1]->s;

    trace("<%s> got /linked %s %s\n", mapper_admin_name(admin),
          sender_name, target_name);

    //TO DO: record link in database

    return 0;
}

/*! Report existing links to the network */
static int handler_device_links_get(const char *path, const char *types,
                                    lo_arg **argv, int argc,
                                    lo_message msg, void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;
    mapper_device md = admin->device;
    mapper_router router = md->routers;

    trace("<%s> got /%s/links/get\n", mapper_admin_name(admin),
          mapper_admin_name(admin));

    /*Search through linked devices */
    while (router != NULL) {
        mapper_admin_send_osc(admin, "/linked", "ss", mapper_admin_name(admin),
                              router->target_name);
        router = router->next;
    }
    return 0;
}

/*! Unlink two devices. */
static int handler_device_unlink(const char *path, const char *types,
                                 lo_arg **argv, int argc, lo_message msg,
                                 void *user_data)
{

    int f = 0;
    const char *sender_name, *target_name;
    mapper_admin admin = (mapper_admin) user_data;
    mapper_device md = admin->device;
    mapper_router router = md->routers;

    if (argc < 2)
        return 0;

    if (types[0] != 's' && types[0] != 'S' && types[1] != 's'
        && types[1] != 'S')
        return 0;

    sender_name = &argv[0]->s;
    target_name = &argv[1]->s;

    trace("<%s> got /unlink %s %s\n", mapper_admin_name(admin),
          sender_name, target_name);

    /*If the device who received the message is the sender in the
     * /unlink message ... */
    if (strcmp(mapper_admin_name(admin), sender_name) == 0) {
        /* Search the router to remove */
        while (router != NULL && f == 0) {
            if (strcmp(router->target_name, target_name) == 0) {
                mdev_remove_router(md, router);
                (*((mapper_admin) user_data)).device->num_routers--;
                /*mapper_router_free(router); */
                f = 1;
            } else
                router = router->next;
        }

        if (f == 1)
            mapper_admin_send_osc(admin, "/unlinked", "ss",
                                  mapper_admin_name(admin), target_name);
    }

    return 0;
}

/*! Respond to /unlinked by removing link from database. */
static int handler_device_unlinked(const char *path, const char *types,
                                   lo_arg **argv, int argc, lo_message msg,
                                   void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;
    const char *sender_name, *target_name;

    if (argc < 2)
        return 0;

    if (types[0] != 's' && types[0] != 'S' && types[1] != 's'
        && types[1] != 'S')
        return 0;

    sender_name = &argv[0]->s;
    target_name = &argv[1]->s;

    trace("<%s> got /unlink %s %s\n", mapper_admin_name(admin),
          sender_name, target_name);

    //TO DO: remove link from database

    return 0;
}

/*! When the /connect message is received by the destination device,
 *  send a connect_to message to the source device. */
static int handler_param_connect(const char *path, const char *types,
                                 lo_arg **argv, int argc, lo_message msg,
                                 void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;

    int md_num_inputs = admin->device->n_inputs;
    mapper_signal *md_inputs = admin->device->inputs;
    int i = 0, f = 0;

    char src_param_name[1024], src_device_name[1024],
    target_param_name[1024], target_device_name[1024];

    if (argc < 2)
        return 0;

    if (types[0] != 's' && types[0] != 'S' && types[1] != 's'
        && types[1] != 'S')
        return 0;

    strcpy(target_device_name, &argv[1]->s);
    strtok(target_device_name, "/");

    // check OSC pattern match
    if (strcmp(mapper_admin_name(admin), target_device_name) == 0) {
        strcpy(target_param_name,
               &argv[1]->s + strlen(target_device_name));
        strcpy(src_device_name, &argv[0]->s);
        strtok(src_device_name, "/");
        strcpy(src_param_name, &argv[0]->s + strlen(src_device_name));

        trace("<%s> got /connect %s%s %s%s\n", mapper_admin_name(admin),
              src_device_name, src_param_name,
              target_device_name, target_param_name);

        while (i < md_num_inputs && f == 0) {

            if (strcmp(md_inputs[i]->name, target_param_name) == 0) {
                f = 1;
                if (argc <= 2) {
                    // use some default arguments related to the signal
                    mapper_admin_send_osc(
                                          admin, "/connect_to", "ss",
                                          strcat(src_device_name, src_param_name),
                                          strcat(target_device_name, target_param_name),
                                          AT_TYPE, md_inputs[i]->type,
                                          md_inputs[i]->minimum ? AT_MIN : -1, md_inputs[i],
                                          md_inputs[i]->maximum ? AT_MAX : -1, md_inputs[i]);
                } else {
                    // add the remaining arguments from /connect
                    mapper_message_t params;
                    if (mapper_msg_parse_params(&params, path, &types[2],
                                                argc-2, &argv[2]))
                        break;
                    mapper_admin_send_osc_with_params(
                                                      admin, &params, "/connect_to", "ss",
                                                      strcat(src_device_name, src_param_name),
                                                      strcat(target_device_name, target_param_name));
                }
            } else
                i++;
        }
    }
    return 0;
}

/*! Connect two signals. */
static int handler_param_connect_to(const char *path, const char *types,
                                    lo_arg **argv, int argc,
                                    lo_message msg, void *user_data)
{

    mapper_admin admin = (mapper_admin) user_data;
    mapper_device md = admin->device;
    mapper_router router = md->routers;

    int md_num_outputs = (*((mapper_admin) user_data)).device->n_outputs;
    mapper_signal *md_outputs =
        (*((mapper_admin) user_data)).device->outputs;

    int i = 0, j = 2, f1 = 0, f2 = 0, recvport = -1, range_update = 0;

    char src_param_name[1024], src_device_name[1024],
        target_param_name[1024], target_device_name[1024], scaling[1024] =
        "dummy", host_address[1024];
    char dest_type;
    float dest_range_min = 0, dest_range_max = 1;
    mapper_clipping_type clip;

    if (argc < 2)
        return 0;

    if ((types[0] != 's' && types[0] != 'S')
        || (types[1] != 's' && types[1] != 'S'))
        return 0;

    strcpy(src_device_name, &argv[0]->s);
    strtok(src_device_name, "/");

    /* Check OSC pattern match */
    if (strcmp(mapper_admin_name(admin), src_device_name) == 0) {

        strcpy(src_param_name, &argv[0]->s + strlen(src_device_name));
        strcpy(target_device_name, &argv[1]->s);
        strtok(target_device_name, "/");
        strcpy(target_param_name,
               &argv[1]->s + strlen(target_device_name));

        trace("<%s> got /connect_to %s%s %s%s + %d arguments\n",
              mapper_admin_name(admin), src_device_name,
              src_param_name, target_device_name, target_param_name, argc);

        /* Searches the source signal among the outputs of the device */
        while (i < md_num_outputs && f1 == 0) {

            /* If the signal exists ... */
            if (strcmp(md_outputs[i]->name, src_param_name) == 0) {
                trace("signal exists: %s\n", md_outputs[i]->name);

                /* Search the router linking to the receiver */
                while (router != NULL && f2 == 0) {
                    if (strcmp(router->target_name, target_device_name) ==
                        0)
                        f2 = 1;
                    else
                        router = router->next;
                }
                f1 = 1;
            } else
                i++;
        }

        /* If the router doesn't exist yet */
        if (f2 == 0) {
            trace("devices are not linked!");
            if (host_address != NULL && recvport != -1) {
                //TO DO: create routed using supplied host and port info
                //TO DO: send /linked message
            } else {
                //TO DO: send /link message to start process - should
                //       also cache /connect_to message for completion after
                //       link???
            }
        }

        /* If this router exists... */
        else {
            if (argc == 2) {
                /* If no properties were provided, default to direct
                 * mapping */
                mapper_router_add_direct_mapping(router,
                                                 admin->device->outputs[i],
                                                 target_param_name);
            } else {
                /* If properties were provided, construct a custom
                 * mapping state: */

                /* Add a flavourless mapping */
                mapper_mapping m = mapper_router_add_blank_mapping(
                    router, admin->device->outputs[i], target_param_name);

                /* Parse the list of properties */
                while (j < argc) {
                    if (types[j] != 's' && types[j] != 'S') {
                        j++;
                    } else if (strcmp(&argv[j]->s, "@type") == 0) {
                        dest_type = argv[j + 1]->c;
                        j += 2;
                    } else if (strcmp(&argv[j]->s, "@min") == 0) {
                        dest_range_min = argv[j + 1]->f;
                        range_update++;
                        j += 2;
                    } else if (strcmp(&argv[j]->s, "@max") == 0) {
                        dest_range_max = argv[j + 1]->f;
                        range_update++;
                        j += 2;
                    } else if (strcmp(&argv[j]->s, "@scaling") == 0) {
                        if (strcmp(&argv[j + 1]->s, "bypass") == 0)
                            m->type = BYPASS;
                        if (strcmp(&argv[j + 1]->s, "linear") == 0)
                            m->type = LINEAR;
                        if (strcmp(&argv[j + 1]->s, "expression") == 0)
                            m->type = EXPRESSION;
                        if (strcmp(&argv[j + 1]->s, "calibrate") == 0)
                            m->type = CALIBRATE;
                        j += 2;
                    } else if (strcmp(&argv[j]->s, "@range") == 0) {
                        if (types[j + 1] == 'i')
                            m->range.src_min = (float) argv[j + 1]->i;
                        else if (types[j + 1] == 'f')
                            m->range.src_min = argv[j + 1]->f;
                        if (types[j + 2] == 'i')
                            m->range.src_max = (float) argv[j + 2]->i;
                        else if (types[j + 2] == 'f')
                            m->range.src_max = argv[j + 2]->f;
                        if (types[j + 3] == 'i')
                            m->range.dest_min = (float) argv[j + 3]->i;
                        else if (types[j + 3] == 'f')
                            m->range.dest_min = argv[j + 3]->f;
                        if (types[j + 4] == 'i')
                            m->range.dest_max = (float) argv[j + 4]->i;
                        else if (types[j + 4] == 'f')
                            m->range.dest_max = argv[j + 4]->f;
                        range_update += 4;
                        j += 5;
                    } else if (strcmp(&argv[j]->s, "@expression") == 0) {
                        char received_expr[1024];
                        strcpy(received_expr, &argv[j + 1]->s);
                        Tree *T = NewTree();
                        int success_tree = get_expr_Tree(T, received_expr);

                        if (success_tree) {
                            free(m->expression);
                            m->expression = strdup(&argv[j + 1]->s);
                            DeleteTree(m->expr_tree);
                            m->expr_tree = T;
                        }
                        j += 2;
                    } else if ((strcmp(&argv[j]->s, "@clipMin") == 0) || (strcmp(&argv[j]->s, "@clipMax") == 0)) {
                        if (strcmp(&argv[j+1]->s, "none") == 0)
                            clip = CT_NONE;
                        else if (strcmp(&argv[j+1]->s, "mute") == 0)
                            clip = CT_MUTE;
                        else if (strcmp(&argv[j+1]->s, "clamp") == 0)
                            clip = CT_CLAMP;
                        else if (strcmp(&argv[j+1]->s, "fold") == 0)
                            clip = CT_FOLD;
                        else if (strcmp(&argv[j+1]->s, "wrap") == 0)
                            clip = CT_WRAP;
                        
                        if (strcmp(&argv[j]->s, "@clipMin") == 0)
                            m->clip_lower = clip;
                        else
                            m->clip_upper = clip;
                        j += 2;
                    } else {
                        j++;
                    }
                }

                if (strcmp(scaling, "dummy") == 0) {
                    if ((range_update == 2)
                        && (md_outputs[i]->type == 'i'
                            || md_outputs[i]->type == 'f')
                        && (dest_type == 'i' || dest_type == 'f')) {
                        /* If destination range was provided and types
                         * are 'i' or 'f', default to linear
                         * mapping. */
                        mapper_router_add_linear_range_mapping(
                            router, admin->device->outputs[i],
                            target_param_name, md_outputs[i]->minimum->f,
                            md_outputs[i]->maximum->f,
                            dest_range_min, dest_range_max);
                    } else {
                        // Otherwise default to direct mapping
                        mapper_router_add_direct_mapping(
                            router, admin->device->outputs[i],
                            target_param_name);
                    }
                }
            }
            /* Add clipping! */
        }
    }
    return 0;
}

/*! Respond to /connected by storing connection in database. */
static int handler_param_connected(const char *path, const char *types,
                                   lo_arg **argv, int argc, lo_message msg,
                                   void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;
    char src_param_name[1024], target_param_name[1024];

    if (argc < 2)
        return 0;

    if (types[0] != 's' && types[0] != 'S' && types[1] != 's'
        && types[1] != 'S')
        return 0;

    strcpy(src_param_name, &argv[0]->s);
    strcpy(target_param_name, &argv[1]->s);

    trace("<%s> got /connected %s %s\n", mapper_admin_name(admin),
          src_param_name, target_param_name);

    //TO DO: record connection in database

    return 0;
}

/*! Modify the connection properties : scaling, range, expression,
 *  clipMin, clipMax. */
static int handler_param_connection_modify(const char *path,
                                           const char *types,
                                           lo_arg **argv, int argc,
                                           lo_message msg, void *user_data)
{

    mapper_admin admin = (mapper_admin) user_data;
    mapper_device md = admin->device;
    mapper_router router = md->routers;

    int md_num_outputs = (*((mapper_admin) user_data)).device->n_outputs;
    mapper_signal *md_outputs =
        (*((mapper_admin) user_data)).device->outputs;

    int i = 0, j = 2, f1 = 0, f2 = 0, range_update = 0;

    char src_param_name[1024], src_device_name[1024],
        target_param_name[1024], target_device_name[1024];
    char mapping_type[1024];
    mapper_clipping_type clip;

    if (argc < 4)
        return 0;

    if ((types[0] != 's' && types[0] != 'S')
        || (types[1] != 's' && types[1] != 'S') || (types[2] != 's'
                                                    && types[2] != 'S'))
        return 0;

    strcpy(src_device_name, &argv[0]->s);
    strtok(src_device_name, "/");

    /* Check OSC pattern match */
    if (strcmp(mapper_admin_name(admin), src_device_name) == 0) {

        strcpy(src_param_name, &argv[0]->s + strlen(src_device_name));
        strcpy(target_device_name, &argv[1]->s);
        strtok(target_device_name, "/");
        strcpy(target_param_name,
               &argv[1]->s + strlen(target_device_name));

        /* Search the source signal among the outputs of the device */
        while (i < md_num_outputs && f1 == 0) {

            /* If this signal exists... */
            if (strcmp(md_outputs[i]->name, src_param_name) == 0) {

                /* Search the router linking to the receiver */
                while (router != NULL && f2 == 0) {
                    if (strcmp(router->target_name, target_device_name) ==
                        0)
                        f2 = 1;
                    else
                        router = router->next;
                }

                /* If this router exists ... */
                if (f2 == 1) {

                    /* Search the mapping corresponding to this connection */
                    mapper_signal_mapping sm = router->mappings;
                    while (sm && sm->signal != md_outputs[i])
                        sm = sm->next;
                    if (!sm)
                        return 0;

                    mapper_mapping m = sm->mapping;
                    while (m && strcmp(m->name, target_param_name) != 0) {
                        m = m->next;
                    }
                    if (!m)
                        return 0;

                    /* Parse the list of properties */
                    while (j < argc) {
                        if (types[j] != 's' && types[j] != 'S') {
                            j++;
                        } else if (strcmp(&argv[j]->s, "@scaling") == 0) {
                            if (strcmp(&argv[j + 1]->s, "bypass") == 0)
                                m->type = BYPASS;
                            if (strcmp(&argv[j + 1]->s, "linear") == 0)
                                m->type = LINEAR;
                            if (strcmp(&argv[j + 1]->s, "expression") == 0)
                                m->type = EXPRESSION;
                            if (strcmp(&argv[j + 1]->s, "calibrate") == 0)
                                m->type = CALIBRATE;
                            j += 2;
                        } else if (strcmp(&argv[j]->s, "@range") == 0) {
                            if (types[j + 1] == 'i')
                                m->range.src_min = (float) argv[j + 1]->i;
                            else if (types[j + 1] == 'f')
                                m->range.src_min = argv[j + 1]->f;
                            if (types[j + 2] == 'i')
                                m->range.src_max = (float) argv[j + 2]->i;
                            else if (types[j + 2] == 'f')
                                m->range.src_max = argv[j + 2]->f;
                            if (types[j + 3] == 'i')
                                m->range.dest_min = (float) argv[j + 3]->i;
                            else if (types[j + 3] == 'f')
                                m->range.dest_min = argv[j + 3]->f;
                            if (types[j + 4] == 'i')
                                m->range.dest_max = (float) argv[j + 4]->i;
                            else if (types[j + 4] == 'f')
                                m->range.dest_max = argv[j + 4]->f;
                            range_update += 4;
                            j += 5;
                        } else if (strcmp(&argv[j]->s, "@expression") == 0) {
                            char received_expr[1024];
                            strcpy(received_expr, &argv[j + 1]->s);
                            Tree *T = NewTree();
                            int success_tree = get_expr_Tree(T, received_expr);
                            
                            if (success_tree) {
                                free(m->expression);
                                m->expression = strdup(&argv[j + 1]->s);
                                DeleteTree(m->expr_tree);
                                m->expr_tree = T;
                            }
                            j += 2;
                        } else if ((strcmp(&argv[j]->s, "@clipMin") == 0) || (strcmp(&argv[j]->s, "@clipMax") == 0)) {
                            if (strcmp(&argv[j+1]->s, "none") == 0)
                                clip = CT_NONE;
                            else if (strcmp(&argv[j+1]->s, "mute") == 0)
                                clip = CT_MUTE;
                            else if (strcmp(&argv[j+1]->s, "clamp") == 0)
                                clip = CT_CLAMP;
                            else if (strcmp(&argv[j+1]->s, "fold") == 0)
                                clip = CT_FOLD;
                            else if (strcmp(&argv[j+1]->s, "wrap") == 0)
                                clip = CT_WRAP;
                            
                            if (strcmp(&argv[j]->s, "@clipMin") == 0)
                                m->clip_lower = clip;
                            else
                                m->clip_upper = clip;
                            j += 2;
                        } else {
                            j++;
                        }
                    }
/***********************TEMPORARY, then only send
                        the modified parameters********************/

                    switch (m->type) {
                    case EXPRESSION:
                        strcpy(mapping_type, "expression");
                        break;

                    case LINEAR:
                        strcpy(mapping_type, "linear");
                        break;

                    case BYPASS:
                        strcpy(mapping_type, "bypass");
                        break;

                    case CALIBRATE:
                        strcpy(mapping_type, "calibrate");
                        break;

                    default:
                        break;

                    }

                    mapper_admin_send_osc(
                        admin, "/connected", "ss",
                        strcat(src_device_name, src_param_name),
                        strcat(target_device_name, target_param_name),
                        AT_SCALING, mapping_type,
                        AT_RANGE,
                            m->range.src_min,  m->range.src_max,
                            m->range.dest_min, m->range.dest_max,
                        AT_EXPRESSION, m->expression,
                        AT_CLIPMIN, "none",
                        AT_CLIPMAX, "none");
                }
                f1 = 1;
            } else
                i++;
        }
    }
    return 0;
}

/*! Disconnect two signals. */
static int handler_param_disconnect(const char *path, const char *types,
                                    lo_arg **argv, int argc,
                                    lo_message msg, void *user_data)
{

    mapper_admin admin = (mapper_admin) user_data;
    mapper_device md = admin->device;
    mapper_router router = md->routers;

    int md_num_outputs = (*((mapper_admin) user_data)).device->n_outputs;
    mapper_signal *md_outputs =
        (*((mapper_admin) user_data)).device->outputs;
    int i = 0, f1 = 0, f2 = 0;

    char src_param_name[1024], src_device_name[1024],
        target_param_name[1024], target_device_name[1024];

    if (argc < 2)
        return 0;

    if (types[0] != 's' && types[0] != 'S' && types[1] != 's'
        && types[1] != 'S')
        return 0;

    strcpy(src_device_name, &argv[0]->s);
    strtok(src_device_name, "/");

    /* Check OSC pattern match */
    if (strcmp(mapper_admin_name(admin), src_device_name) == 0) {

        strcpy(src_param_name, &argv[0]->s + strlen(src_device_name));
        strcpy(target_device_name, &argv[1]->s);
        strtok(target_device_name, "/");
        strcpy(target_param_name,
               &argv[1]->s + strlen(target_device_name));

        trace("<%s> got /disconnect %s%s %s%s\n",
              mapper_admin_name(admin), src_device_name,
              src_param_name, target_device_name, target_param_name);

        /* Searches the source signal among the outputs of the device */
        while (i < md_num_outputs && f1 == 0) {
            /* If this signal exists ... */
            if (strcmp(md_outputs[i]->name, src_param_name) == 0) {

                /* Searches the router linking to the receiver */
                while (router != NULL && f2 == 0) {
                    if (strcmp(router->target_name, target_device_name) ==
                        0)
                        f2 = 1;
                    else
                        router = router->next;
                }

                /* If this router exists ... */
                if (f2 == 1) {
                    /* Search the mapping corresponding to this connection */
                    mapper_signal_mapping sm = router->mappings;
                    while (sm && sm->signal != md_outputs[i])
                        sm = sm->next;
                    if (!sm)
                        return 0;

                    mapper_mapping m = sm->mapping;
                    while (m && strcmp(m->name, target_param_name) != 0) {
                        m = m->next;
                    }
                    if (!m)
                        return 0;

                    /*The mapping is removed */
                    mapper_router_remove_mapping( /*router, */ sm, m);
                } else
                    return 0;
                f1 = 1;
            } else
                i++;
        }
    }
    return 0;
}

/*! Respond to /disconnected by removing connection from database. */
static int handler_param_disconnected(const char *path, const char *types,
                                      lo_arg **argv, int argc,
                                      lo_message msg, void *user_data)
{
    mapper_admin admin = (mapper_admin) user_data;
    char src_param_name[1024], target_param_name[1024];

    if (argc < 2)
        return 0;

    if (types[0] != 's' && types[0] != 'S' && types[1] != 's'
        && types[1] != 'S')
        return 0;

    strcpy(src_param_name, &argv[0]->s);
    strcpy(target_param_name, &argv[1]->s);

    trace("<%s> got /disconnected %s %s\n", mapper_admin_name(admin),
          src_param_name, target_param_name);

    //TO DO: remove connection from database

    return 0;
}

/*! Report existing connections to the network */
static int handler_device_connections_get(const char *path,
                                          const char *types,
                                          lo_arg **argv, int argc,
                                          lo_message msg, void *user_data)
{
    char src_name[256], target_name[256];
    int i = 0;
    mapper_admin admin = (mapper_admin) user_data;
    mapper_device md = admin->device;
    mapper_router router = md->routers;

    int md_num_outputs = (*((mapper_admin) user_data)).device->n_outputs;
    mapper_signal *md_outputs =
        (*((mapper_admin) user_data)).device->outputs;

    while (i < md_num_outputs) {

        /* Searches the router linking to the receiver */
        while (router != NULL) {
            mapper_signal_mapping sm = router->mappings;
            snprintf(src_name, 256, "/%s.%d%s",
                     (*((mapper_admin) user_data)).identifier,
                     (*((mapper_admin) user_data)).ordinal.value,
                     md_outputs[i]->name);
            while (sm != NULL) {
                mapper_mapping m = sm->mapping;
                snprintf(target_name, 256, "%s%s", router->target_name,
                         m->name);
                mapper_admin_send_osc(admin, "/connected", "ss",
                                      src_name, target_name);
                sm = sm->next;
            }
            router = router->next;
        }
        i++;
    }


    return 0;
}
