#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <zlib.h>

#include "mapper_internal.h"

#define AUTOSUBSCRIBE_INTERVAL 60
extern const char* network_message_strings[NUM_MSG_STRINGS];

mapper_db mapper_db_new(mapper_network net, int subscribe_flags)
{
    if (!net)
        net = mapper_network_new(0, 0, 0);
    if (!net)
        return 0;

    net->own_network = 0;
    mapper_db db = mapper_network_add_db(net);

    if (subscribe_flags) {
        mapper_db_subscribe(db, 0, subscribe_flags, -1);
    }
    return db;
}

void mapper_db_free(mapper_db db)
{
    if (!db)
        return;

    // remove callbacks now so they won't be called when removing devices
    mapper_db_remove_all_callbacks(db);

    mapper_network_remove_db(db->network);

    // unsubscribe from and remove any autorenewing subscriptions
    while (db->subscriptions) {
        mapper_db_unsubscribe(db, db->subscriptions->device);
    }

    /* Remove all non-local maps */
    mapper_map *maps = mapper_db_maps(db);
    while (maps) {
        mapper_map map = *maps;
        maps = mapper_map_query_next(maps);
        if (!map->local)
            mapper_db_remove_map(db, map);
    }

    /* Remove all non-local devices and signals from the database except for
     * those referenced by local maps. */
    mapper_device *devs = mapper_db_devices(db);
    while (devs) {
        mapper_device dev = *devs;
        devs = mapper_device_query_next(devs);
        if (dev->local)
            continue;

        int no_local_device_maps = 1;
        mapper_signal *sigs = mapper_db_device_signals(db, dev, MAPPER_DIR_ANY);
        while (sigs) {
            mapper_signal sig = *sigs;
            sigs = mapper_signal_query_next(sigs);
            int no_local_signal_maps = 1;
            mapper_map *maps = mapper_db_signal_maps(db, sig, MAPPER_DIR_ANY);
            while (maps) {
                if ((*maps)->local) {
                    no_local_device_maps = no_local_signal_maps = 0;
                    mapper_map_query_done(maps);
                    break;
                }
                maps = mapper_map_query_next(maps);
            }
            if (no_local_signal_maps)
                mapper_db_remove_signal(db, sig);
        }
        if (no_local_device_maps)
            mapper_db_remove_device(db, dev, 1);
    }

    if (!db->network->device && !db->network->own_network)
        mapper_network_free(db->network);
}

mapper_network mapper_db_network(mapper_db db)
{
    return db->network;
}

void mapper_db_set_timeout(mapper_db db, int timeout_sec)
{
    if (timeout_sec < 0)
        timeout_sec = MAPPER_TIMEOUT_SEC;
    db->timeout_sec = timeout_sec;
}

int mapper_db_timeout(mapper_db db)
{
    return db->timeout_sec;
}

void mapper_db_flush(mapper_db db, int timeout_sec, int quiet)
{
    mapper_clock_now(&db->network->clock, &db->network->clock.now);

    // flush expired device records
    mapper_device dev;
    uint32_t last_ping = db->network->clock.now.sec - timeout_sec;
    while ((dev = mapper_db_expired_device(db, last_ping))) {
        // also need to remove subscriptions
        mapper_subscription *s = &db->subscriptions;
        while (*s) {
            if ((*s)->device == dev) {
                // don't bother sending '/unsubscribe' since device is unresponsive
                // remove from subscriber list
                mapper_subscription temp = *s;
                *s = temp->next;
                free(temp);
            }
            else
                s = &(*s)->next;
        }
        mapper_db_remove_device(db, dev, quiet);
    }
}

void mapper_db_sync(mapper_db db)
{
    mapper_network_set_dest_bus(db->network);

//    // Update devices
//    mapper_device dev = db->devices;
//    while (dev) {
//        mapper_device_sync(dev);
//        dev = mapper_list_next(dev);
//    }
//
//    // Update signal
//    mapper_signal sig = db->signals;
//    while (sig) {
//        mapper_signal_sync(sig);
//        sig = mapper_list_next(sig);
//    }

    // Update maps
    mapper_map map = db->maps;
    while (map) {
        mapper_map_sync(map);
        map = mapper_list_next(map);
    }
    return;
}

/* Generic index and lookup functions to which the above tables would be passed.
 * These are called for specific types below. */

int mapper_db_property_index(const void *thestruct, table extra,
                             unsigned int index, const char **property,
                             int *length, char *type, const void **value,
                             table proptable)
{
    die_unless(type!=0, "type parameter cannot be null.\n");
    die_unless(value!=0, "value parameter cannot be null.\n");
    die_unless(length!=0, "length parameter cannot be null.\n");

    int i=0, j=0;

    /* Unfortunately due to "optional" properties like minimum/maximum, unit,
     * etc, we cannot use an O(1) lookup here--the index changes according to
     * availability of properties.  Thus, we have to search through properties
     * linearly, incrementing a counter along the way, so indexed lookup is
     * O(N).  Meaning iterating through all indexes is O(N^2).  A better way
     * would be to use an iterator-style interface if efficiency was important
     * for iteration. */

    /* First search static properties */
    property_table_value_t *prop;
    for (i=0; i < proptable->len; i++)
    {
        prop = table_value_at_index_p(proptable, i);
        if (prop->indirect) {
            void **pp = (void**)((char*)thestruct + prop->offset);
            if (*pp) {
                if (j==index) {
                    if (property)
                        *property = table_key_at_index(proptable, i);
                    if (prop->type == 'o')
                        *type = *((char*)thestruct + prop->alt_type);
                    else
                        *type = prop->type;
                    if (prop->length > 0)
                        *length = *(int*)((char*)thestruct + prop->length);
                    else
                        *length = prop->length * -1;
                    if (prop->type == 's' && prop->length > 0 && *length == 1) {
                        // In this case pass the char* rather than the array
                        char **temp = *pp;
                        *value = temp[0];
                    }
                    else
                        *value = *pp;
                    return 0;
                }
                j++;
            }
        }
        else {
            if (j==index) {
                if (property)
                    *property = table_key_at_index(proptable, i);
                if (prop->type == 'o')
                    *type = *((char*)thestruct + prop->alt_type);
                else
                    *type = prop->type;
                *value = (lo_arg*)((char*)thestruct + prop->offset);
                if (prop->length > 0)
                    *length = *(int*)((char*)thestruct + prop->length);
                else
                    *length = prop->length * -1;
                return 0;
            }
            j++;
        }
    }

    if (extra) {
        index -= j;
        mapper_property_value_t *val;
        val = table_value_at_index_p(extra, index);
        if (val) {
            if (property)
                *property = table_key_at_index(extra, index);
            *type = val->type;
            *value = val->value;
            *length = val->length;
            return 0;
        }
    }

    return 1;
}

int mapper_db_property(const void *thestruct, table extra, const char *property,
                       int *length, char *type, const void **value,
                       table proptable)
{
    die_unless(type!=0, "type parameter cannot be null.\n");
    die_unless(value!=0, "value parameter cannot be null.\n");
    die_unless(length!=0, "length parameter cannot be null.\n");

    const mapper_property_value_t *val;
    if (extra) {
        val = table_find_p(extra, property);
        if (val) {
            *type = val->type;
            *value = val->value;
            *length = val->length;
            return 0;
        }
    }

    property_table_value_t *prop;
    prop = table_find_p(proptable, property);
    if (prop) {
        if (prop->type == 'o')
            *type = *((char*)thestruct + prop->alt_type);
        else
            *type = prop->type;
        if (prop->length > 0)
            *length = *(int*)((char*)thestruct + prop->length);
        else
            *length = prop->length * -1;
        if (prop->indirect) {
            void **pp = (void**)((char*)thestruct + prop->offset);
            if (*pp) {
                *value = *pp;
            }
            else
                return 1;
        }
        else
            *value = (void*)((char*)thestruct + prop->offset);
        return 0;
    }
    return 1;
}

static void add_callback(fptr_list *head, const void *f, const void *user)
{
    fptr_list cb = (fptr_list)malloc(sizeof(struct _fptr_list));
    cb->f = (void*)f;
    cb->context = (void*)user;
    cb->next = *head;
    *head = cb;
}

static void remove_callback(fptr_list *head, const void *f, const void *user)
{
    fptr_list cb = *head;
    fptr_list prevcb = 0;
    while (cb) {
        if (cb->f == f && cb->context == user)
            break;
        prevcb = cb;
        cb = cb->next;
    }
    if (!cb)
        return;

    if (prevcb)
        prevcb->next = cb->next;
    else
        *head = cb->next;

    free(cb);
}

/**** Device records ****/

mapper_device mapper_db_add_or_update_device_params(mapper_db db,
                                                    const char *name,
                                                    mapper_message_t *params)
{
    const char *no_slash = skip_slash(name);
    mapper_device dev = mapper_db_device_by_name(db, no_slash);
    int rc = 0, updated = 0;

    if (!dev) {
        dev = (mapper_device)mapper_list_add_item((void**)&db->devices,
                                                  sizeof(*dev));
        dev->name = strdup(no_slash);
        dev->id = crc32(0L, (const Bytef *)no_slash, strlen(no_slash)) << 32;
        dev->db = db;
        dev->extra = table_new();
        dev->updater = table_new();
        rc = 1;
    }

    if (dev) {
        updated = mapper_device_set_from_message(dev, params);
        mapper_clock_now(&db->network->clock, &db->network->clock.now);
        mapper_timetag_copy(&dev->synced, db->network->clock.now);

        if (rc || updated) {
            fptr_list cb = db->device_callbacks;
            while (cb) {
                mapper_db_device_handler *h = cb->f;
                h(dev, rc ? MAPPER_ADDED : MAPPER_MODIFIED, cb->context);
                cb = cb->next;
            }
        }
    }

    return dev;
}

// Internal function called by /logout protocol handler
void mapper_db_remove_device(mapper_db db, mapper_device dev, int quiet)
{
    if (!dev)
        return;

    mapper_db_remove_maps_by_query(db, mapper_db_device_maps(db, dev, 0));

    mapper_db_remove_signals_by_query(db, mapper_db_device_signals(db, dev,
                                                                   MAPPER_DIR_ANY));

    mapper_list_remove_item((void**)&db->devices, dev);

    if (!quiet) {
        fptr_list cb = db->device_callbacks;
        while (cb) {
            mapper_db_device_handler *h = cb->f;
            h(dev, MAPPER_REMOVED, cb->context);
            cb = cb->next;
        }
    }

    if (dev->identifier)
        free(dev->identifier);
    if (dev->name)
        free(dev->name);
    if (dev->description)
        free(dev->description);
    if (dev->host)
        free(dev->host);
    if (dev->lib_version && dev->lib_version != PACKAGE_VERSION)
        free(dev->lib_version);
    if (dev->extra)
        table_free(dev->extra);
    mapper_list_free_item(dev);
}

mapper_device *mapper_db_devices(mapper_db db)
{
    return mapper_list_from_data(db->devices);
}

static int cmp_query_local_devices(const void *context_data, mapper_device dev)
{
    return dev->local != 0;
}

mapper_device *mapper_db_local_devices(mapper_db db)
{
    return ((mapper_device *)
            mapper_list_new_query(db->devices, cmp_query_local_devices, "i", 0));
}

mapper_device mapper_db_device_by_name(mapper_db db, const char *name)
{
    const char *no_slash = skip_slash(name);
    mapper_device dev = db->devices;
    while (dev) {
        if (strcmp(dev->name, no_slash)==0)
            return dev;
        dev = mapper_list_next(dev);
    }
    return 0;
}

mapper_device mapper_db_device_by_id(mapper_db db, uint64_t id)
{
    mapper_device dev = db->devices;
    while (dev) {
        if (id == dev->id)
            return dev;
        dev = mapper_list_next(dev);
    }
    return 0;
}

static int cmp_query_devices_by_name_match(const void *context_data,
                                           mapper_device dev)
{
    const char *pattern = (const char*)context_data;
    return strstr(dev->name, pattern)!=0;
}

mapper_device *mapper_db_devices_by_name_match(mapper_db db, const char *pattern)
{
    return ((mapper_device *)
            mapper_list_new_query(db->devices, cmp_query_devices_by_name_match,
                                  "s", pattern));
}

static inline int check_type(char type)
{
    return strchr("ifdsct", type) != 0;
}

static int compare_value(mapper_op op, int length, int type, const void *val1,
                         const void *val2)
{
    int i, compare = 0, difference = 0;
    switch (type) {
        case 's':
            if (length == 1)
                compare = strcmp((const char*)val1, (const char*)val2);
            else {
                for (i = 0; i < length; i++) {
                    compare += strcmp(((const char**)val1)[i],
                                      ((const char**)val2)[i]);
                    difference += abs(compare);
                }
            }
            break;
        case 'i':
            for (i = 0; i < length; i++) {
                compare += ((int*)val1)[i] > ((int*)val2)[i];
                compare -= ((int*)val1)[i] < ((int*)val2)[i];
                difference += abs(compare);
            }
            break;
        case 'f':
            for (i = 0; i < length; i++) {
                compare += ((float*)val1)[i] > ((float*)val2)[i];
                compare -= ((float*)val1)[i] < ((float*)val2)[i];
                difference += abs(compare);
            }
            break;
        case 'd':
            for (i = 0; i < length; i++) {
                compare += ((double*)val1)[i] > ((double*)val2)[i];
                compare -= ((double*)val1)[i] < ((double*)val2)[i];
                difference += abs(compare);
            }
            break;
        case 'c':
            for (i = 0; i < length; i++) {
                compare += ((char*)val1)[i] > ((char*)val2)[i];
                compare -= ((char*)val1)[i] < ((char*)val2)[i];
                difference += abs(compare);
            }
            break;
        case 't':
            for (i = 0; i < length; i++) {
                compare += ((uint64_t*)val1)[i] > ((uint64_t*)val2)[i];
                compare -= ((uint64_t*)val1)[i] < ((uint64_t*)val2)[i];
                difference += abs(compare);
            }
            break;
        case 'h':
            for (i = 0; i < length; i++) {
                compare += ((char*)val1)[i] > ((char*)val2)[i];
                compare -= ((char*)val1)[i] < ((char*)val2)[i];
                difference += abs(compare);
            }
            break;
        default:
            return 0;
    }
    switch (op) {
        case MAPPER_OP_EQUAL:
            return compare == 0 && !difference;
        case MAPPER_OP_GREATER_THAN:
            return compare > 0;
        case MAPPER_OP_GREATER_THAN_OR_EQUAL:
            return compare >= 0;
        case MAPPER_OP_LESS_THAN:
            return compare < 0;
        case MAPPER_OP_LESS_THAN_OR_EQUAL:
            return compare <= 0;
        case MAPPER_OP_NOT_EQUAL:
            return compare != 0 || difference;
        default:
            return 0;
    }
}

static int cmp_query_devices_by_property(const void *context_data,
                                         mapper_device dev)
{
    int op = *(int*)context_data;
    int length = *(int*)(context_data + sizeof(int));
    char type = *(char*)(context_data + sizeof(int) * 2);
    void *value = *(void**)(context_data + sizeof(int) * 3);
    const char *property = (const char*)(context_data+sizeof(int)*3+sizeof(void*));
    int _length;
    char _type;
    const void *_value;
    if (mapper_device_property(dev, property, &_length, &_type, &_value))
        return (op == MAPPER_OP_DOES_NOT_EXIST);
    if (op == MAPPER_OP_EXISTS)
        return 1;
    if (op == MAPPER_OP_DOES_NOT_EXIST)
        return 0;
    if (_type != type || _length != length)
        return 0;
    return compare_value(op, length, type, _value, value);
}

mapper_device *mapper_db_devices_by_property(mapper_db db, const char *property,
                                             int length, char type,
                                             const void *value, mapper_op op)
{
    if (!property || !check_type(type) || length < 1)
        return 0;
    if (op <= MAPPER_OP_UNDEFINED || op >= NUM_MAPPER_OPS)
        return 0;
    return ((mapper_device *)
            mapper_list_new_query(db->devices, cmp_query_devices_by_property,
                                  "iicvs", op, length, type, &value, property));
}

void mapper_db_add_device_callback(mapper_db db, mapper_db_device_handler *h,
                                   const void *user)
{
    add_callback(&db->device_callbacks, h, user);
}

void mapper_db_remove_device_callback(mapper_db db, mapper_db_device_handler *h,
                                      const void *user)
{
    remove_callback(&db->device_callbacks, h, user);
}

void mapper_db_check_device_status(mapper_db db, uint32_t time_sec)
{
    time_sec -= db->timeout_sec;
    mapper_device dev = db->devices;
    while (dev) {
        // check if device has "checked in" recently
        // this could be /sync ping or any sent metadata
        if (dev->synced.sec && (dev->synced.sec < time_sec)) {
            fptr_list cb = db->device_callbacks;
            while (cb) {
                mapper_db_device_handler *h = cb->f;
                h(dev, MAPPER_EXPIRED, cb->context);
                cb = cb->next;
            }
        }
        dev = mapper_list_next(dev);
    }
}

mapper_device mapper_db_expired_device(mapper_db db, uint32_t last_ping)
{
    mapper_device dev = db->devices;
    while (dev) {
        if (dev->synced.sec && (dev->synced.sec < last_ping)) {
            return dev;
        }
        dev = mapper_list_next(dev);
    }
    return 0;
}

/**** Signals ****/

mapper_signal mapper_db_add_or_update_signal_params(mapper_db db,
                                                    const char *name,
                                                    const char *device_name,
                                                    mapper_message_t *msg)
{
    mapper_signal sig = 0;
    int rc = 0, updated = 0;

    mapper_device dev = mapper_db_device_by_name(db, device_name);
    if (dev) {
        sig = mapper_db_device_signal_by_name(db, dev, name);
        if (sig && sig->local)
            return sig;
    }
    else
        dev = mapper_db_add_or_update_device_params(db, device_name, 0);

    if (!sig) {
        sig = (mapper_signal)mapper_list_add_item((void**)&db->signals,
                                                  sizeof(mapper_signal_t));

        // also add device record if necessary
        sig->device = dev;

        // Defaults (int, length=1)
        mapper_signal_init(sig, name, 1, 'i', 0, 0, 0, 0, 0, 0);

        rc = 1;
    }

    if (sig) {
        updated = mapper_signal_set_from_message(sig, msg);

        if (rc || updated) {
            // TODO: Should we really allow callbacks to free themselves?
            fptr_list cb = db->signal_callbacks, temp;
            while (cb) {
                temp = cb->next;
                mapper_db_signal_handler *h = cb->f;
                h(sig, rc ? MAPPER_ADDED : MAPPER_MODIFIED, cb->context);
                cb = temp;
            }
        }
    }
    return sig;
}

void mapper_db_add_signal_callback(mapper_db db, mapper_db_signal_handler *h,
                                   const void *user)
{
    add_callback(&db->signal_callbacks, h, user);
}

void mapper_db_remove_signal_callback(mapper_db db, mapper_db_signal_handler *h,
                                      const void *user)
{
    remove_callback(&db->signal_callbacks, h, user);
}

static int cmp_query_signals(const void *context_data, mapper_signal sig)
{
    int direction = *(int*)context_data;
    return !direction || (sig->direction & direction);
}

mapper_signal *mapper_db_signals(mapper_db db, mapper_direction dir)
{
    if (!dir)
        return mapper_list_from_data(db->signals);
    return ((mapper_signal *)
            mapper_list_new_query(db->signals, cmp_query_signals, "i", dir));
}

mapper_signal mapper_db_signal_by_id(mapper_db db, uint64_t id)
{
    mapper_signal sig = db->signals;
    if (!sig)
        return 0;

    while (sig) {
        if (sig->id == id)
            return sig;
        sig = mapper_list_next(sig);
    }
    return 0;
}

static int cmp_query_signals_by_name(const void *context_data,
                                     mapper_signal sig)
{
    int direction = *(int*)context_data;
    const char *name = (const char*)(context_data + sizeof(int));
    return ((!direction || (sig->direction & direction))
            && (strcmp(sig->name, name)==0));
}

mapper_signal *mapper_db_signals_by_name(mapper_db db, const char *name)
{
    return ((mapper_signal *)
            mapper_list_new_query(db->signals, cmp_query_signals_by_name,
                                  "is", MAPPER_DIR_ANY, name));
}

static int cmp_query_signals_by_name_match(const void *context_data,
                                           mapper_signal sig)
{
    const char *pattern = (const char*)(context_data);
    return (strstr(sig->name, pattern)!=0);
}

mapper_signal *mapper_db_signals_by_name_match(mapper_db db, const char *pattern)
{
    return ((mapper_signal *)
            mapper_list_new_query(db->signals, cmp_query_signals_by_name_match,
                                  "s", pattern));
}

static int cmp_query_signals_by_property(const void *context_data,
                                         mapper_signal sig)
{
    int op = *(int*)context_data;
    int length = *(int*)(context_data + sizeof(int));
    char type = *(char*)(context_data + sizeof(int) * 2);
    void *value = *(void**)(context_data + sizeof(int) * 3);
    const char *property = (const char*)(context_data+sizeof(int)*3+sizeof(void*));
    int _length;
    char _type;
    const void *_value;
    if (mapper_signal_property(sig, property, &_length, &_type, &_value))
        return (op == MAPPER_OP_DOES_NOT_EXIST);
    if (op == MAPPER_OP_EXISTS)
        return 1;
    if (op == MAPPER_OP_DOES_NOT_EXIST)
        return 0;
    if (_type != type || _length != length)
        return 0;
    return compare_value(op, length, type, _value, value);
}

mapper_signal *mapper_db_signals_by_property(mapper_db db, const char *property,
                                             int length, char type,
                                             const void *value, mapper_op op)
{
    if (!property || !check_type(type) || length < 1)
        return 0;
    if (op <= MAPPER_OP_UNDEFINED || op >= NUM_MAPPER_OPS)
        return 0;
    return ((mapper_signal *)
            mapper_list_new_query(db->signals, cmp_query_signals_by_property,
                                  "iicvs", op, length, type, &value, property));
}

static int cmp_query_device_signals(const void *context_data, mapper_signal sig)
{
    uint64_t dev_id = *(int64_t*)context_data;
    int direction = *(int*)(context_data + sizeof(uint64_t));
    return ((!direction || (sig->direction & direction))
            && (dev_id == sig->device->id));
}

mapper_signal *mapper_db_device_signals(mapper_db db, mapper_device dev,
                                        mapper_direction dir)
{
    if (!dev)
        return 0;
    return ((mapper_signal *)
            mapper_list_new_query(db->signals, cmp_query_device_signals,
                                  "hi", dev->name ? dev->id : 0, dir));
}

mapper_signal mapper_db_device_signal_by_name(mapper_db db, mapper_device dev,
                                              const char *sig_name)
{
    if (!dev)
        return 0;
    mapper_signal sig = db->signals;
    if (!sig)
        return 0;

    while (sig) {
        if ((sig->device == dev) && strcmp(sig->name, skip_slash(sig_name))==0)
            return sig;
        sig = mapper_list_next(sig);
    }
    return 0;
}

mapper_signal mapper_db_device_signal_by_index(mapper_db db, mapper_device dev,
                                               mapper_direction dir, int index)
{
    if (!dev || index < 0)
        return 0;
    mapper_signal sig = db->signals;
    if (!sig)
        return 0;

    int count = -1;
    while (sig && count < index) {
        if ((sig->device == dev) && (!dir || (sig->direction & dir))) {
            if (++count == index)
                return sig;
        }
        sig = mapper_list_next(sig);
    }
    return 0;
}

void mapper_db_remove_signal(mapper_db db, mapper_signal sig)
{
    // remove any stored maps using this signal
    mapper_db_remove_maps_by_query(db, mapper_db_signal_maps(db, sig, 0));

    mapper_list_remove_item((void**)&db->signals, sig);

    fptr_list cb = db->signal_callbacks;
    while (cb) {
        mapper_db_signal_handler *h = cb->f;
        h(sig, MAPPER_REMOVED, cb->context);
        cb = cb->next;
    }

    if (sig->direction & MAPPER_INCOMING)
        sig->device->num_inputs--;
    if (sig->direction & MAPPER_OUTGOING)
        sig->device->num_outputs--;

    mapper_signal_free(sig);

    mapper_list_free_item(sig);
}

// Internal function called by /logout protocol handler.
void mapper_db_remove_signal_by_name(mapper_db db, const char *device_name,
                                     const char *signal_name)
{
    mapper_device dev = mapper_db_device_by_name(db, device_name);
    if (!dev)
        return;
    mapper_signal sig = mapper_db_device_signal_by_name(db, dev, signal_name);
    if (sig && !sig->local)
        mapper_db_remove_signal(db, sig);
}

void mapper_db_remove_signals_by_query(mapper_db db, mapper_signal *query)
{
    while (query) {
        mapper_signal sig = *query;
        query = mapper_signal_query_next(query);
        if (!sig->local)
            mapper_db_remove_signal(db, sig);
    }
}

/**** Map records ****/

static int compare_slot_names(const void *l, const void *r)
{
    int result = strcmp(((mapper_slot)l)->signal->device->name,
                        ((mapper_slot)r)->signal->device->name);
    if (result == 0)
        return strcmp(((mapper_slot)l)->signal->name,
                      ((mapper_slot)r)->signal->name);
    return result;
}

mapper_map mapper_db_add_or_update_map_params(mapper_db db, int num_sources,
                                              const char **src_names,
                                              const char *dest_name,
                                              mapper_message_t *params)
{
    if (num_sources >= MAX_NUM_MAP_SOURCES) {
        trace("error: maximum mapping sources exceeded.\n");
        return 0;
    }

    mapper_map map = 0;
    int rc = 0, updated = 0, devnamelen, i, j;
    char *devnamep, *signame, devname[256];

    /* We could be part of larger "convergent" mapping, so we will retrieve
     * record by mapping id instead of names. */
    int64_t id = 0;
    if (params && mapper_message_param_if_int64(params, AT_ID, &id)) {
        trace("no 'id' property found in map metadata, aborting.\n");
        return 0;
    }
    map = mapper_db_map_by_id(db, id);

    if (!map) {
        map = (mapper_map)mapper_list_add_item((void**)&db->maps,
                                               sizeof(mapper_map_t));
        map->db = db;
        map->num_sources = num_sources;
        map->sources = (mapper_slot) calloc(1, sizeof(struct _mapper_slot)
                                            * num_sources);
        for (i = 0; i < num_sources; i++) {
            devnamelen = mapper_parse_names(src_names[i], &devnamep, &signame);
            if (!devnamelen || devnamelen >= 256) {
                trace("error extracting device name\n");
                // clean up partially-built record
                mapper_list_remove_item((void**)&db->maps, map);
                mapper_list_free_item(map);
                return 0;
            }
            strncpy(devname, devnamep, devnamelen);
            devname[devnamelen] = 0;

            // also add source signal if necessary
            map->sources[i].signal =
                mapper_db_add_or_update_signal_params(db, signame, devname, 0);
            map->sources[i].id = i;
            map->sources[i].causes_update = 1;
            map->sources[i].map = map;
            if (map->sources[i].signal->local) {
                map->sources[i].num_instances = map->sources[i].signal->num_instances;
                map->sources[i].use_as_instance = map->sources[i].num_instances > 1;
            }
        }
        devnamelen = mapper_parse_names(dest_name, &devnamep, &signame);
        if (!devnamelen || devnamelen >= 256) {
            trace("error extracting device name\n");
            // clean up partially-built record
            mapper_list_remove_item((void**)&db->maps, map);
            mapper_list_free_item(map);
            return 0;
        }
        strncpy(devname, devnamep, devnamelen);
        devname[devnamelen] = 0;
        map->destination.map = map;

        // also add destination signal if necessary
        map->destination.signal =
            mapper_db_add_or_update_signal_params(db, signame, devname, 0);
        map->destination.causes_update = 1;
        if (map->destination.signal->local) {
            map->destination.num_instances = map->destination.signal->num_instances;
            map->destination.use_as_instance = map->destination.num_instances > 1;
        }

        map->extra = table_new();
        map->updater = table_new();
        rc = 1;
    }
    else if (map->num_sources < num_sources) {
        // add one or more sources
        for (i = 0; i < num_sources; i++) {
            devnamelen = mapper_parse_names(src_names[i], &devnamep, &signame);
            if (!devnamelen || devnamelen >= 256) {
                trace("error extracting device name\n");
                return 0;
            }
            strncpy(devname, devnamep, devnamelen);
            devname[devnamelen] = 0;
            for (j = 0; j < map->num_sources; j++) {
                if (strlen(map->sources[j].signal->device->name) == devnamelen
                    && strcmp(devname, map->sources[j].signal->device->name)==0
                    && strcmp(signame, map->sources[j].signal->name)==0) {
                    map->sources[j].id = i;
                    break;
                }
            }
            if (j == map->num_sources) {
                map->num_sources++;
                map->sources = realloc(map->sources, sizeof(struct _mapper_slot)
                                       * map->num_sources);
                map->sources[j].signal =
                    mapper_db_add_or_update_signal_params(db, signame, devname, 0);
                map->sources[j].id = i;
                map->sources[j].causes_update = 1;
                if (map->sources[j].signal->local) {
                    map->sources[j].num_instances = map->sources[j].signal->num_instances;
                    map->sources[j].use_as_instance = map->sources[j].num_instances > 1;
                }
            }
        }
        // slots should be in alphabetical order
        qsort(map->sources, map->num_sources,
              sizeof(mapper_slot_t), compare_slot_names);
    }

    if (map) {
        updated = mapper_map_set_from_message(map, params, 0);

        if (rc || updated) {
            fptr_list cb = db->map_callbacks;
            while (cb) {
                mapper_map_handler *h = cb->f;
                h(map, rc ? MAPPER_ADDED : MAPPER_MODIFIED, cb->context);
                cb = cb->next;
            }
        }
    }

    return map;
}

void mapper_db_add_map_callback(mapper_db db, mapper_map_handler *h,
                                const void *user)
{
    add_callback(&db->map_callbacks, h, user);
}

void mapper_db_remove_map_callback(mapper_db db, mapper_map_handler *h,
                                   const void *user)
{
    remove_callback(&db->map_callbacks, h, user);
}

mapper_map *mapper_db_maps(mapper_db db)
{
    return mapper_list_from_data(db->maps);
}

mapper_map mapper_db_map_by_id(mapper_db db, uint64_t id)
{
    mapper_map map = db->maps;
    if (!map)
        return 0;
    while (map) {
        if (map->id == id)
            return map;
        map = mapper_list_next(map);
    }
    return 0;
}

static int cmp_query_maps_by_property(const void *context_data, mapper_map map)
{
    int op = *(int*)context_data;
    int length = *(int*)(context_data + sizeof(int));
    char type = *(char*)(context_data + sizeof(int) * 2);
    void *value = *(void**)(context_data + sizeof(int) * 3);
    const char *property = (const char*)(context_data+sizeof(int)*3+sizeof(void*));
    int _length;
    char _type;
    const void *_value;
    if (mapper_map_property(map, property, &_length, &_type, &_value))
        return (op == MAPPER_OP_DOES_NOT_EXIST);
    if (op == MAPPER_OP_EXISTS)
        return 1;
    if (op == MAPPER_OP_DOES_NOT_EXIST)
        return 0;
    if (_type != type || _length != length)
        return 0;
    return compare_value(op, length, type, _value, value);
}

mapper_map *mapper_db_maps_by_property(mapper_db db, const char *property,
                                       int length, char type, const void *value,
                                       mapper_op op)
{
    if (!property || !check_type(type) || length < 1)
        return 0;
    if (op <= MAPPER_OP_UNDEFINED || op >= NUM_MAPPER_OPS)
        return 0;
    return ((mapper_map *)
            mapper_list_new_query(db->maps, cmp_query_maps_by_property,
                                  "iicvs", op, length, type, &value, property));
}

static int cmp_query_maps_by_slot_property(const void *context_data,
                                           mapper_map map)
{
    int i, direction = *(int*)context_data;
    int op = *(int*)(context_data + sizeof(int));
    int length2, length1 = *(int*)(context_data + sizeof(int) * 2);
    char type2, type1 = *(char*)(context_data + sizeof(int) * 3);
    const void *value2, *value1 = *(void**)(context_data + sizeof(int) * 4);
    const char *property = (const char*)(context_data + sizeof(int) * 4
                                         + sizeof(void*));
    if (!direction || direction & MAPPER_INCOMING) {
        if (!mapper_slot_property(&map->destination, property, &length2,
                                  &type2, &value2)
            && type1 == type2 && length1 == length2
            && compare_value(op, length1, type1, value2, value1))
            return 1;
    }
    if (!direction || direction & MAPPER_OUTGOING) {
        for (i = 0; i < map->num_sources; i++) {
            if (!mapper_slot_property(&map->sources[i], property, &length2,
                                      &type2, &value2)
                && type1 == type2 && length1 == length2
                && compare_value(op, length1, type1, value2, value1))
                return 1;
        }
    }
    return 0;
}

mapper_map *mapper_db_maps_by_slot_property(mapper_db db, const char *property,
                                            int length, char type,
                                            const void *value, mapper_op op)
{
    if (!property || !check_type(type) || length < 1)
        return 0;
    if (op <= MAPPER_OP_UNDEFINED || op >= NUM_MAPPER_OPS)
        return 0;
    return ((mapper_map *)
            mapper_list_new_query(db->maps, cmp_query_maps_by_slot_property,
                                  "iiicvs", 0, op, length, type,
                                  &value, property));
}

mapper_map *mapper_db_maps_by_src_slot_property(mapper_db db,
                                                const char *property,
                                                int length, char type,
                                                const void *value,
                                                mapper_op op)
{
    if (!property || !check_type(type) || length < 1)
        return 0;
    if (op <= MAPPER_OP_UNDEFINED || op >= NUM_MAPPER_OPS)
        return 0;
    return ((mapper_map *)
            mapper_list_new_query(db->maps, cmp_query_maps_by_slot_property,
                                  "iiicvs", MAPPER_OUTGOING, op, length, type,
                                  &value, property));
}

mapper_map *mapper_db_maps_by_dest_slot_property(mapper_db db,
                                                 const char *property,
                                                 int length, char type,
                                                 const void *value,
                                                 mapper_op op)
{
    if (!property || !check_type(type) || length < 1)
        return 0;
    if (op <= MAPPER_OP_UNDEFINED || op >= NUM_MAPPER_OPS)
        return 0;
    return ((mapper_map *)
            mapper_list_new_query(db->maps, cmp_query_maps_by_slot_property,
                                  "iiicvs", MAPPER_INCOMING, op, length, type,
                                  &value, property));
}

static int cmp_query_device_maps(const void *context_data, mapper_map map)
{
    uint64_t dev_id = *(uint64_t*)context_data;
    int direction = *(int*)(context_data + sizeof(uint64_t));
    if (!direction || (direction & MAPPER_OUTGOING)) {
        int i;
        for (i = 0; i < map->num_sources; i++) {
            if (map->sources[i].signal->device->id == dev_id)
                return 1;
        }
    }
    if (!direction || (direction & MAPPER_INCOMING)) {
        if (map->destination.signal->device->id == dev_id)
            return 1;
    }
    return 0;
}

mapper_map *mapper_db_device_maps(mapper_db db, mapper_device dev,
                                  mapper_direction dir)
{
    if (!dev)
        return 0;
    return ((mapper_map *)
            mapper_list_new_query(db->maps, cmp_query_device_maps,
                                  "hi", dev->id, dir));
}

static int cmp_query_signal_maps(const void *context_data, mapper_map map)
{
    mapper_signal sig = *(mapper_signal *)context_data;
    int direction = *(int*)(context_data + sizeof(int64_t));
    if (!direction || (direction & MAPPER_OUTGOING)) {
        int i;
        for (i = 0; i < map->num_sources; i++) {
            if (map->sources[i].signal == sig)
                return 1;
        }
    }
    if (!direction || (direction & MAPPER_INCOMING)) {
        if (map->destination.signal == sig)
            return 1;
    }
    return 0;
}

mapper_map *mapper_db_signal_maps(mapper_db db, mapper_signal sig,
                                  mapper_direction dir)
{
    if (!sig)
        return 0;
    return ((mapper_map *)
            mapper_list_new_query(db->maps, cmp_query_signal_maps,
                                  "vi", &sig, dir));
}

void mapper_db_remove_maps_by_query(mapper_db db, mapper_map_t **maps)
{
    while (maps) {
        mapper_map map = *maps;
        maps = mapper_map_query_next(maps);
        if (!map->local)
            mapper_db_remove_map(db, map);
    }
}

static void free_slot(mapper_slot slot)
{
    if (slot->minimum)
        free(slot->minimum);
    if (slot->maximum)
        free(slot->maximum);
}

void mapper_db_remove_map(mapper_db db, mapper_map map)
{
    int i;
    if (!map)
        return;

    mapper_list_remove_item((void**)&db->maps, map);

    fptr_list cb = db->map_callbacks;
    while (cb) {
        mapper_map_handler *h = cb->f;
        h(map, MAPPER_REMOVED, cb->context);
        cb = cb->next;
    }

    if (map->sources) {
        for (i = 0; i < map->num_sources; i++) {
            free_slot(&map->sources[i]);
        }
        free(map->sources);
    }
    free_slot(&map->destination);
    if (map->scope.size && map->scope.devices) {
        free(map->scope.devices);
    }
    if (map->expression)
        free(map->expression);
    if (map->extra)
        table_free(map->extra);
    if (map->updater)
        table_free(map->updater);
    mapper_list_free_item(map);
}

void mapper_db_remove_all_callbacks(mapper_db db)
{
    fptr_list cb;
    while ((cb = db->device_callbacks)) {
        db->device_callbacks = db->device_callbacks->next;
        free(cb);
    }
    while ((cb = db->signal_callbacks)) {
        db->signal_callbacks = db->signal_callbacks->next;
        free(cb);
    }
    while ((cb = db->map_callbacks)) {
        db->map_callbacks = db->map_callbacks->next;
        free(cb);
    }
}

void mapper_db_dump(mapper_db db)
{
#ifdef DEBUG
    mapper_device dev = db->devices;
    printf("Registered devices:\n");
    while (dev) {
        mapper_device_pp(dev);
        dev = mapper_list_next(dev);
    }

    mapper_signal sig = db->signals;
    printf("Registered signals:\n");
    while (sig) {
        mapper_signal_pp(sig, 1);
        sig = mapper_list_next(sig);
    }

    mapper_map map = db->maps;
    printf("Registered maps:\n");
    while (map) {
        mapper_map_pp(map);
        map = mapper_list_next(map);
    }
#endif
}

static void set_network_dest(mapper_db db, mapper_device dev)
{
    // TODO: look up device info, maybe send directly
    mapper_network_set_dest_bus(db->network);
}

static void subscribe_internal(mapper_db db, mapper_device dev, int flags,
                               int timeout, int version)
{
    char cmd[1024];
    snprintf(cmd, 1024, "/%s/subscribe", dev->name);

    set_network_dest(db, dev);
    lo_message m = lo_message_new();
    if (m) {
        if (flags & SUBSCRIBE_ALL)
            lo_message_add_string(m, "all");
        else {
            if (flags & SUBSCRIBE_DEVICE)
                lo_message_add_string(m, "device");
            if (flags & SUBSCRIBE_DEVICE_SIGNALS)
                lo_message_add_string(m, "signals");
            else {
                if (flags & SUBSCRIBE_DEVICE_INPUTS)
                    lo_message_add_string(m, "inputs");
                else if (flags & SUBSCRIBE_DEVICE_OUTPUTS)
                    lo_message_add_string(m, "outputs");
            }
            if (flags & SUBSCRIBE_DEVICE_MAPS)
                lo_message_add_string(m, "maps");
            else {
                if (flags & SUBSCRIBE_DEVICE_MAPS_IN)
                    lo_message_add_string(m, "incoming_maps");
                else if (flags & SUBSCRIBE_DEVICE_MAPS_OUT)
                    lo_message_add_string(m, "outgoing_maps");
            }
        }
        lo_message_add_string(m, "@lease");
        lo_message_add_int32(m, timeout);
        if (version >= 0) {
            lo_message_add_string(m, "@version");
            lo_message_add_int32(m, version);
        }
        mapper_network_add_message(db->network, cmd, 0, m);
        mapper_network_send(db->network);
    }
}

static void unsubscribe_internal(mapper_db db, mapper_device dev,
                                 int send_message)
{
    char cmd[1024];
    // check if autorenewing subscription exists
    mapper_subscription *s = &db->subscriptions;
    while (*s) {
        if ((*s)->device == dev) {
            if (send_message) {
                snprintf(cmd, 1024, "/%s/unsubscribe", dev->name);
                set_network_dest(db, dev);
                lo_message m = lo_message_new();
                if (!m) {
                    trace("couldn't allocate lo_message\n");
                    break;
                }
                mapper_network_add_message(db->network, cmd, 0, m);
                mapper_network_send(db->network);
            }
            // remove from subscriber list
            mapper_subscription temp = *s;
            *s = temp->next;
            free(temp);
            return;
        }
        s = &(*s)->next;
    }
}

int mapper_db_update(mapper_db db, int block_ms)
{
    int ping_time = db->network->clock.next_ping;
    int count = mapper_network_poll(db->network);
    mapper_clock_now(&db->network->clock, &db->network->clock.now);

    // check if any subscriptions need to be renewed
    mapper_subscription s = db->subscriptions;
    while (s) {
        if (s->lease_expiration_sec < db->network->clock.now.sec) {
            subscribe_internal(db, s->device, s->flags,
                               AUTOSUBSCRIBE_INTERVAL, -1);
            // leave 10-second buffer for subscription renewal
            s->lease_expiration_sec = (db->network->clock.now.sec
                                       + AUTOSUBSCRIBE_INTERVAL - 10);
        }
        s = s->next;
    }

    if (block_ms) {
        double then = mapper_get_current_time();
        while ((mapper_get_current_time() - then)*1000 < block_ms) {
            count += mapper_network_poll(db->network);
#ifdef WIN32
            Sleep(block_ms);
#else
            usleep(block_ms * 100);
#endif
        }
    }

    if (ping_time != db->network->clock.next_ping) {
        // some housekeeping: check if any devices have timed out
        mapper_db_check_device_status(db, db->network->clock.now.sec);
    }

    return count;
}

static void on_device_autosubscribe(mapper_device dev, mapper_record_action a,
                                    const void *user)
{
    mapper_db db = (mapper_db)(user);

    // New subscriptions are handled in network.c as response to "sync" msg
    if (a == MAPPER_REMOVED) {
        unsubscribe_internal(db, dev, 0);
    }
}

static void mapper_db_autosubscribe(mapper_db db, int flags)
{
    // TODO: remove autorenewing subscription record if necessary
    if (!db->autosubscribe && flags) {
        mapper_db_add_device_callback(db, on_device_autosubscribe, db);
        mapper_db_request_devices(db);
    }
    else if (db->autosubscribe && !flags) {
        mapper_db_remove_device_callback(db, on_device_autosubscribe, db);
        while (db->subscriptions) {
            unsubscribe_internal(db, db->subscriptions->device, 1);
        }
    }
    db->autosubscribe = flags;
}

static mapper_subscription subscription(mapper_db db, mapper_device dev)
{
    mapper_subscription s = db->subscriptions;
    while (s) {
        if (s->device == dev)
            return s;
        s = s->next;
    }
    return 0;
}

void mapper_db_subscribe(mapper_db db, mapper_device dev, int flags, int timeout)
{
    if (!dev) {
        mapper_db_autosubscribe(db, flags);
        return;
    }
    if (timeout == -1) {
        // special case: autorenew subscription lease
        // first check if subscription already exists
        mapper_subscription s = subscription(db, dev);

        if (!s) {
            // store subscription record
            s = malloc(sizeof(struct _mapper_subscription));
            s->device = dev;
            s->next = db->subscriptions;
            db->subscriptions = s;
        }
        s->flags = flags;

        mapper_clock_now(&db->network->clock, &db->network->clock.now);
        // leave 10-second buffer for subscription lease
        s->lease_expiration_sec = (db->network->clock.now.sec
                                   + AUTOSUBSCRIBE_INTERVAL - 10);

        timeout = AUTOSUBSCRIBE_INTERVAL;
    }

    subscribe_internal(db, dev, flags, timeout, 0);
}

void mapper_db_unsubscribe(mapper_db db, mapper_device dev)
{
    if (!dev)
        mapper_db_autosubscribe(db, SUBSCRIBE_NONE);
    unsubscribe_internal(db, dev, 1);
}

void mapper_db_request_devices(mapper_db db)
{
    lo_message msg = lo_message_new();
    if (!msg) {
        trace("couldn't allocate lo_message\n");
        return;
    }
    mapper_network_set_dest_bus(db->network);
    mapper_network_add_message(db->network, 0, MSG_WHO, msg);
}
