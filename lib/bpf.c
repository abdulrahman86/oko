/*
 * Copyright 2018 Orange
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>
#include <errno.h>

#include <config.h>
#include "bpf.h"

#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(bpf)

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

#define MAX_PRINTF_LENGTH 80

static void register_functions(struct ubpf_vm *vm);

struct ubpf_vm *
create_ubpf_vm(const ovs_be16 filter_prog)
{
    struct ubpf_vm *vm = ubpf_create(filter_prog);
    if (!vm) {
        VLOG_WARN_RL(&rl, "Failed to create VM\n");
        return NULL;
    }

    register_functions(vm);

    return vm;
}

bool
load_filter_prog(struct ubpf_vm *vm, size_t code_len, char *code)
{
    char *errmsg;
    int rv = ubpf_load_elf(vm, code, code_len, &errmsg);
    if (rv < 0) {
        VLOG_WARN_RL(&rl, "Failed to load code: %s\n", errmsg);
        free(errmsg);
        return false;
    }

    ubpf_jit_fn fn = ubpf_compile(vm, &errmsg);
    if (fn == NULL) {
        VLOG_WARN_RL(&rl, "Failed to compile: %s\n", errmsg);
        free(errmsg);
        return false;
    }

    return true;
}

struct filter_prog *
filter_prog_chain_lookup(struct ovs_list **filter_prog_chain,
                         const ovs_be16 fp_instance_id, int last_fp_pos)
{
    struct filter_prog *fp;
    int i = 0;
    if (!filter_prog_chain) {
        return NULL;
    }
    if (!*filter_prog_chain) {
        return NULL;
    }
    LIST_FOR_EACH (fp, filter_prog_node, *filter_prog_chain) {
        i++;
        if (fp_instance_id == fp->fp_instance_id) {
            if (last_fp_pos != i) {
                break;
            }
            return fp;
        }
    }
    return NULL;
}

bool
filter_prog_chain_add(struct ovs_list **filter_prog_chain,
                      const ovs_be16 fp_instance_id, struct ubpf_vm *vm,
                      bpf_result expected_result)
{
    if (*filter_prog_chain == NULL) {
        *filter_prog_chain = xmalloc(sizeof(struct ovs_list));
        ovs_list_init(*filter_prog_chain);
    }
    bool found = false;
    struct filter_prog *fp;
    LIST_FOR_EACH (fp, filter_prog_node, *filter_prog_chain) {
        if (fp_instance_id == fp->fp_instance_id) {
            found = true;
        }
    }
    if (!found) {
        struct filter_prog *node = xmalloc(sizeof(struct filter_prog));
        node->fp_instance_id = fp_instance_id;
        node->vm = vm;
        node->expected_result = expected_result;
        ovs_list_push_back(*filter_prog_chain, &node->filter_prog_node);
    }
    return !found;
}

void
filter_prog_chain_free(struct ovs_list *filter_prog_chain)
{
    if (filter_prog_chain) {
        struct filter_prog *fp;
        LIST_FOR_EACH_POP (fp, filter_prog_node, filter_prog_chain) {
            free(fp);
        }
        free(filter_prog_chain);
    }
}

void *
ubpf_map_lookup(const struct ubpf_map *map, void *key)
{
    if (OVS_UNLIKELY(!map)) {
        return NULL;
    }
    if (OVS_UNLIKELY(!map->ops.map_lookup)) {
        return NULL;
    }
    if (OVS_UNLIKELY(!key)) {
        return NULL;
    }
    return map->ops.map_lookup(map, key);
}

struct ubpf_func_proto ubpf_map_lookup_proto = {
    .func = (ext_func)ubpf_map_lookup,
    .arg_types = {
                MAP_PTR,
                PKT_PTR | MAP_VALUE_PTR | STACK_PTR | UNKNOWN,
                0xff,
                0xff,
                0xff,
            },
    .arg_sizes = {
                0xff,
                SIZE_MAP_KEY,
                0xff,
                0xff,
                0xff,
            },
    .ret = MAP_VALUE_PTR | NULL_VALUE,
};

int
ubpf_map_update(struct ubpf_map *map, const void *key, void *item)
{
    if (OVS_UNLIKELY(!map)) {
        return -1;
    }
    if (OVS_UNLIKELY(!map->ops.map_update)) {
        return -2;
    }
    if (OVS_UNLIKELY(!key)) {
        return -3;
    }
    if (OVS_UNLIKELY(!item)) {
        return -4;
    }
    return map->ops.map_update(map, key, item);
}

struct ubpf_func_proto ubpf_map_update_proto = {
    .func = (ext_func)ubpf_map_update,
    .arg_types = {
                MAP_PTR,
                PKT_PTR | MAP_VALUE_PTR | STACK_PTR,
                PKT_PTR | MAP_VALUE_PTR | STACK_PTR,
                0xff,
                0xff,
            },
    .arg_sizes = {
                0xff,
                SIZE_MAP_KEY,
                SIZE_MAP_VALUE,
                0xff,
                0xff,
            },
    .ret = UNKNOWN,
};

static int
ubpf_map_add(struct ubpf_map *map, void *item)
{
    if (OVS_UNLIKELY(!map)) {
        return -1;
    }
    if (OVS_UNLIKELY(!map->ops.map_add)) {
        return -2;
    }
    if (OVS_UNLIKELY(!item)) {
        return -3;
    }
    return map->ops.map_add(map, item);
}

struct ubpf_func_proto ubpf_map_add_proto = {
    .func = (ext_func)ubpf_map_add,
    .arg_types = {
                MAP_PTR,
                PKT_PTR | MAP_VALUE_PTR | STACK_PTR,
                0xff,
                0xff,
                0xff,
            },
    .arg_sizes = {
                0xff,
                SIZE_MAP_VALUE,
                0xff,
                0xff,
                0xff,
            },
    .ret = UNKNOWN,
};

static int
ubpf_map_delete(struct ubpf_map *map, const void *key)
{
    if (OVS_UNLIKELY(!map)) {
        return -1;
    }
    if (OVS_UNLIKELY(!map->ops.map_delete)) {
        return -2;
    }
    if (OVS_UNLIKELY(!key)) {
        return -3;
    }
    return map->ops.map_delete(map, key);
}

struct ubpf_func_proto ubpf_map_delete_proto = {
    .func = (ext_func)ubpf_map_delete,
    .arg_types = {
                MAP_PTR,
                PKT_PTR | MAP_VALUE_PTR | STACK_PTR,
                0xff,
                0xff,
                0xff,
            },
    .arg_sizes = {
                0xff,
                SIZE_MAP_KEY,
                0xff,
                0xff,
                0xff,
            },
    .ret = UNKNOWN,
};

static void
ubpf_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char str[MAX_PRINTF_LENGTH];
    if (vsnprintf(str, MAX_PRINTF_LENGTH, fmt, args) >= 0)
        VLOG_ERR("%s", str);
    va_end(args);
}

struct ubpf_func_proto ubpf_printf_proto = {
    .func = (ext_func)ubpf_printf,
    .arg_types = {
                0xff,
                0xff,
                0xff,
                0xff,
                0xff,
            },
    .arg_sizes = {
                0xff,
                0xff,
                0xff,
                0xff,
                0xff,
            },
    .ret = UNINIT,
};

static uint64_t
ubpf_time_get_ns(void)
{
    struct timespec curr_time = {0, 0};
    uint64_t curr_time_ns = 0;
    clock_gettime(CLOCK_REALTIME, &curr_time);
    curr_time_ns = curr_time.tv_nsec + curr_time.tv_sec * 1.0e9;
    return curr_time_ns;
}

struct ubpf_func_proto ubpf_time_get_ns_proto = {
    .func = (ext_func)ubpf_time_get_ns,
    .arg_types = {
                0xff,
                0xff,
                0xff,
                0xff,
                0xff,
            },
    .arg_sizes = {
                0xff,
                0xff,
                0xff,
                0xff,
                0xff,
            },
    .ret = UNKNOWN,
};

static uint32_t
ubpf_hash(void *item, uint64_t size)
{
    return hashlittle(item, (uint32_t)size, 0);
}

struct ubpf_func_proto ubpf_hash_proto = {
    .func = (ext_func)ubpf_hash,
    .arg_types = {
                PKT_PTR | MAP_VALUE_PTR | STACK_PTR,
                IMM,
                0xff,
                0xff,
                0xff,
            },
    .arg_sizes = {
                SIZE_PTR_MAX,
                SIZE_64,
                0xff,
                0xff,
                0xff,
            },
    .ret = UNKNOWN,
};

static void
register_functions(struct ubpf_vm *vm)
{
    ubpf_register_function(vm, 1, "ubpf_map_lookup", ubpf_map_lookup_proto);
    ubpf_register_function(vm, 2, "ubpf_map_update", ubpf_map_update_proto);
    ubpf_register_function(vm, 3, "ubpf_map_delete", ubpf_map_delete_proto);
    ubpf_register_function(vm, 4, "ubpf_map_add", ubpf_map_add_proto);
    ubpf_register_function(vm, 5, "ubpf_time_get_ns", ubpf_time_get_ns_proto);
    ubpf_register_function(vm, 6, "ubpf_hash", ubpf_hash_proto);
    ubpf_register_function(vm, 7, "ubpf_printf", ubpf_printf_proto);
}
