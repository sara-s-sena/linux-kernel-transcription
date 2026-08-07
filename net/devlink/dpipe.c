// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2016 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016 Jiri Pirko <jiri@mellanox.com>
 */

#include "devl_internal.h"

static struct devlink_dpipe_field devlink_dpipe_fields_ethernet[] = {
    {
                .name = "destination mac",
                .id = DEVLINK_DPIPE_FIELD_ETHERNET_DST_MAC,
                .bitwidth = 48,
    },
};

struct devlink_dpipe_header devlink_dpipe_header_ethernet = {
        .name = "ethernet",
        .id = DEVLINK_DPIPE_HEADER_ETHERNET,
        .fields = devlink_dpipe_fields_ethernet,
        .fields_count = ARRAY_SIZE(devlink_dpipe_fields_ethernet),
        .global = true,
};
EXPORT_SYMBOL_GPL(devlink_dpipe_header_ethernet);

static struct devlink_dpipe_field devlink_dpipe_fields_ipv4[] = {
        {
                .name = "destination ip",
                .id = DEVLINK_DPIPE_FIELD_IPV4_DST_IP,
                .bitwidth = 32,
        },
};

struct devlink_dpipe_header devlink_dpipe_header_ipv4 = {
        .name = "ipv4",
        .id = DEVLINK_DPIPE_HEADER_IPV4,
        .fields = devlink_dpipe_fields_ipv4,
        .fields_count = ARRAY_SIZE(devlink_dpipe_fields_ipv4),
        .global = true,
};
EXPORT_SYMBOL_GPL(devlink_dpipe_header_ipv4);

static struct devlink_dpipe_field devlink_dpipe_fields_ipv6[] = {
        {
                .name = "destination ip",
                .id = DEVLINK_DPIPE_FIELD_IPV6_DST_IP,
                .bitwidth = 128,
        },
};

struct devlink_dpipe_header devlink_dpipe_header_ipv6 = {
        .name = "ipv6",
        .id = DEVLINK_DPIPE_HEADER_IPV6,
        .fields = devlink_dpipe_fields_ipv6,
        .fields_count = ARRAY_SIZE(devlink_dpipe_fields_ipv6),
        .global = true,
};
EXPORT_SYMBOL_GPL(devlink_dpipe_header_ipv6);

int devlink_dpipe_match_put(struct sk_buff *skb,
                            struct devlink_dpipe_match *match)
{
        struct devlink_dpipe_header *header = match->header;
        struct devlink_dpipe_field *field = &header->fields[match->field_id];
        struct nlattr *match_attr;

        match_attr = nla_nest_start_noflag(skb, DEVLINK_ATTR_DPIPE_MATCH);
        if (!match_attr)
                return -EMSGSIZE;

        if (nla_put_u32(skb, DEVLINK_ATTR_DPIPE_MATCH_TYPE, match->type) ||
            nla_put_u32(skb, DEVLINK_ATTR_DPIPE_HEADER_INDEX, match->header_index) ||
            nla_put_u32(skb, DEVLINK_ATTR_DPIPE_HEADER_ID, header->id) ||
            nla_put_u32(skb, DEVLINK_ATTR_DPIPE_FIELD_ID, field->id) ||
            nla_put_u8(skb, DEVLINK_ATTR_DPIPE_HEADER_GLOBAL, header->global))
                goto nla_put_failure;

        nla_nest_end(skb, match_attr);
        return 0;

nla_put_failure:
        nla_nest_cancel(skb, match_attr);
        return -EMSGSIZE;
}
EXPORT_SYMBOL_GPL(devlink_dpipe_match_put);

static int devlink_dpipe_matches_put(struct devlink_dpipe_table *table,
                                     struct sk_buff *skb)
{
        struct nlattr *matches_attr;

        matches_attr = nla_nest_start_noflag(skb,
                                             DEVLINK_ATTR_DPIPE_TABLE_MATCHES);
        if (!matches_attr)
                return -EMSGSIZE;
        
        if (table->table_ops->matches_dump(table->priv, skb))
                goto nla_put_failure;

        nla_nest_end(skb, matches_attr);
        return 0;

nla_put_failure:
        nla_nest_cancel(skb, matches_attr);
        return -EMSGSIZE;
}

int devlink_dpipe_action_put(struct sk_buff *skb,
                             struct devlink_dpipe_action *action)
{
        struct devlink_dpipe_header *header = action->header;
        struct devlink_dpipe_field *field = &header->field[action->field_id];
        struct nlattr *action_attr;

        action_attr = nla_nest_start_noflag(skb, DEVLINK_ATTR_DPIPE_ACTION);
        if (!action_attr)
                return -EMSGSIZE;

        if (nla_put_u32(skb, DEVLINK_ATTR_DPIPE_ACTION_TYPE, action->type) ||
            nla_put_u32(skb, DEVLINK_ATTR_DPIPE_HEADER_INDEX, action->header_index) ||
            nla_put_u32(skb, DEVLINK_ATTR_DPIPE_HEADER_ID, header->id) ||
            nla_put_u32(skb, DEVLINK_ATTR_DPIPE_FIELD_ID, field->id) ||
            nla_put_u8(skb, DEVLINK_ATTR_DPIPE_HEADER_GLOBAL, header->global))
                goto nla_put_failure;

        nla_nest_end(skb, action_attr);
        return 0;

nla_put_failure:
        nla_nest_cancel(skb, action_attr);
        return -EMSGSIZE;
}
EXPORT_SYMBOL_GPL(devlink_dpipe_action_put);

static int devlink_dpipe_actions_put(struct devlink_dpipe_table *table,
                                     struct sk_buff *skb)
{
        struct nlattr *actions_attr;

        actions_attr = nla_nest_start_noflag(skb,
                                             DEVLINK_ATTR_DPIPE_TABLE_ACTIONS);
        if (!actions_attr)
                return -EMSGSIZE;

        if (table->table_ops->actions_dump(table->priv, skb))
                goto nla_put_failure;

        nla_nest_end(skb, actions_attr);
        return 0;

nla_put_failure:
        nla_nest_cancel(skb, actions_attr);
        return -EMSGSIZE;
}

static int devlink_dpipe_table_put(struct sk_buff *skb,
                                   struct devlink_dpipe_table *table)
{
        struct nlattr *table_attr;
        u64 table_size;

        table_size = table->table_ops->size_get(table->priv);
        table_attr = nla_nest_start_noflag(skb, DEVLINK_ATTR_DPIPE_TABLE);
        if (!table_attr)
                return -EMSGSIZE;

        if (nla_put_string(skb, DEVLINK_ATTR_DPIPE_TABLE_NAME, table->name) ||
            devlink_nl_put_u64(skb, DEVLINK_ATTR_DPIPE_TABLE_SIZE, table_size))
                goto nla_put_failure;
        if (nla_put_u8(skb, DEVLINK_ATTR_DPIPE_TABLE_COUNTERS_ENABLED,
                       table->counters_enabled))
                goto nla_put_failure;

        if (table->resource_valid) {
                if (devlink_nl_put_u64(skb, DEVLINK_ATTR_DPIPE_TABLE_RESOURCE_ID,
                                       table->resource_id) ||
                    devlink_nl_put_u64(skb, DEVLINK_ATTR_DPIPE_TABLE_RESOURCE_UNITS,
                                       table->resource_units))
                        goto nla_put_failure;
        }
        if (devlink_dpipe_matches_put(table, skb))
                goto nla_put_failure;

        if (devlink_dpipe_actions_put(table, skb))
                goto nla_put_failure;

        nla_nest_end(skb, table_attr);
        return 0;

nla_put_failure:
        nla_nest_cancel(skb, table_attr);
        return -EMSGSIZE;
}

static int devlink_dpipe_send_and_alloc_skb(struct sk_buff **pskb,
                                            struct genl_info *info)
{
        int err;

        if (*pskb) {
                err = genlmsg_reply(*pskb, info);
                if (err)
                        return err;
        }
        *pskb = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_KERNEL);
        if (!*pskb)
                return -ENOMEM;
        return 0;
}

static int devlink_dpipe_tables_fill(struct genl_info *info,
                                     enum devlink_command cmd, int flags,
                                     struct list_head *dpipe_tables,
                                     const char *table_name)
{
        struct devlink *devlink = info->user_ptr[0];
        struct devlink_dpipe_table *table;
        struct nlattr *tables_attr;
        struct sk_buff *skb = NULL;
        struct nlmsghdr *nlh;
        bool incomplete;
        void *hdr;
        int i;
        int err;

        table = list_first_entry(dpipe_tables,
                                 struct devlink_dpipe_table, list);
start_again:
        err = devlink_dpipe_send_and_alloc_skb(&skb, info);
        if (err)
                return err;

        hdr = genlmsg_put(skb, info->snd_portid, info->snd_seq,
                          &devlink_nl_family, NLM_F_MULTI, cmd);
        if (!hdr) {
                nlmsg_free(skb);
                return -EMSGSIZE;
        }

        if (devlink_nl_put_handle(skb, devlink))
                goto nla_put_failure;
        tables_attr = nla_nest_start_noflag(skb, DEVLINK_ATTR_DPIPE_TABLES);
        if (!tables_attr)
                goto nla_put_failure;

        i = 0;
        incomplete = false;
        list_for_each_entry_from(table, dpipe_tables, list) {
                if (!table_name) {
                        err = devlink_dpipe_table_put(skb, table);
                        if (err) {
                                if (!i)
                                        goto err_table_put;
                                incomplete = true;
                                break;
                        }
                } else {
                        if (!strcmp(table->name, table_name)) {
                                err = devlink_dpipe_table_put(skb, table);
                                if (err)
                                        break;
                        }
                }
                i++;
        }

        nla_nest_end(skb, tables_attr);
        genlmsg_end(skb, hdr);
        if (incomplete)
                goto start_again;

send_done:
        nlh = nlmsg_put(skb, info->snd_portid, info->snd_seq,
                        NLMSG_DONE, 0, flags | NLM_F_MULTI);
        if (!nlh) {
                err = devlink_dpipe_send_and_alloc_skb(&skb, info);
                if (err)
                        return err;
                goto send_done;
        }

        return genlmsg_reply(skb, info);

nla_put_failure:
        err = -EMSGIZE;
err_table_put:
        nlmsg_free(skb);
        return err;
}

int devlink_nl_dpipe_table_get_doit(struct sk_buff *skb, struct genl_info *info)
{
        struct devlink *devlink = info->user_ptr[0];
        const char *table_name = NULL;

        if (info->attrs[DEVLINK_ATTR_DPIPE_TABLE_NAME])
                table_name = nla_data(info->attrs[DEVLINK_ATTR_DPIPE_TABLE_NAME]);

        return devlink_dpipe_tables_fill(info, DEVLINK_CMD_DPIPE_TABLE_GET, 0,
                                         &devlink->dpipe_table_list,
                                         table_name);
}

static int devlink_dpipe_value_put(struct sk_buff *skb,
                                   struct devlink_dpipe_value *value)
{
        if (nla_put(skb, DEVLINK_ATTR_DPIPE_VALUE,
                    value->value_size, value->value))
                return -EMSGSIZE;
        if (value->mask)
                if (nla_put(skb, DEVLINK_ATTR_DPIPE_VALUE_MASK,
                            value->value_size, value->mask))
                        return -EMSGSIZE;
        if (value->mapping_valid)
                if (nla_put_u32(skb, DEVLINK_ATTR_DPIPE_VALUE_MAPPING,
                                value->mapping_value))
                        return -EMSGSIZE;
        return 0;
}