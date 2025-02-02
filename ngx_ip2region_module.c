#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ip2region.h"
#include "ngx_ip2region.h"


static void *ngx_ip2region_create_conf(ngx_cycle_t *cycle);
static char *ngx_ip2region_init_conf(ngx_cycle_t *cycle, void *conf);

static ngx_int_t ngx_ip2region_init_process(ngx_cycle_t *cycle);
static void ngx_ip2region_exit_process(ngx_cycle_t *cycle);

/**
 * 新加的方法
 */
static ngx_int_t ngx_http_ipdb_add_variables(ngx_conf_t *cf);

static ngx_int_t ngx_http_ip2region_region_name(ngx_http_request_t *r,
                                         ngx_http_variable_value_t *v, uintptr_t data);

static ngx_int_t ngx_http_ip2region_country_name(ngx_http_request_t *r,
                                ngx_http_variable_value_t *v, uintptr_t data);

datablock_entry * ngx_ip2region_get_datablock(ngx_str_t *addr_text);



static ip2region_entry g_ip2region_entry;


search_func_ptr ip2region_search_func;


#define NGX_IP2REGION_MEMORY 0
#define NGX_IP2REGION_BINARY 1
#define NGX_IP2REGION_BTREE 2

static ngx_conf_enum_t  ngx_ip2region_search_algo[] = {
    { ngx_string("memory"), NGX_IP2REGION_MEMORY },
    { ngx_string("binary"), NGX_IP2REGION_BINARY },
    { ngx_string("btree"), NGX_IP2REGION_BTREE},
    { ngx_null_string, 0 }
};


static ngx_command_t ngx_ip2region_commands[] = {

    {
        ngx_string("ip2region_db_file"),
        NGX_MAIN_CONF | NGX_DIRECT_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        0,
        offsetof(ngx_ip2region_conf_t, db_file),
        NULL
    },

    {
        ngx_string("ip2region_algo"),
        NGX_MAIN_CONF | NGX_DIRECT_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_enum_slot,
        0,
        offsetof(ngx_ip2region_conf_t, algo),
        &ngx_ip2region_search_algo
    },

    ngx_null_command
};


static ngx_core_module_t  ngx_ip2region_module_ctx = {
    ngx_string("ip2region"),
    ngx_ip2region_create_conf,
    ngx_ip2region_init_conf,
};


ngx_module_t  ngx_ip2region_module = {
    NGX_MODULE_V1,
    &ngx_ip2region_module_ctx,             /* module context */
    ngx_ip2region_commands,                /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_ip2region_init_process,            /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_ip2region_exit_process,            /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static void *
ngx_ip2region_create_conf(ngx_cycle_t *cycle)
{
    ngx_ip2region_conf_t  *ip2region_conf;
    ip2region_conf = ngx_pcalloc(cycle->pool, sizeof(ngx_ip2region_conf_t));

    if(ip2region_conf == NULL) {
        return NULL;
    }

    ip2region_conf->algo = NGX_CONF_UNSET_UINT;
    return ip2region_conf;
}

static char *
ngx_ip2region_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_ip2region_conf_t  *ip2region_conf = (ngx_ip2region_conf_t *)conf;

    if(ip2region_conf->db_file.len == 0) {
        //ngx_str_set(&ip2region_conf->db_file, "conf/ip2region.db");
        return NGX_CONF_OK;
    }

    if(ngx_conf_full_name(cycle, &ip2region_conf->db_file, 0) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    ngx_log_debug1(NGX_LOG_INFO, ngx_cycle->log, 0, "%V", &ip2region_conf->db_file);

    if(ip2region_conf->algo == NGX_CONF_UNSET_UINT) {
        ip2region_conf->algo = NGX_IP2REGION_MEMORY;
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_ip2region_init_process(ngx_cycle_t *cycle)
{
    ngx_ip2region_conf_t *conf = (ngx_ip2region_conf_t *)ngx_get_conf(ngx_cycle->conf_ctx, ngx_ip2region_module);

    if(conf->db_file.len == 0) {
        ngx_log_debug0(NGX_LOG_INFO, ngx_cycle->log, 0, "ip2region not init");
        return NGX_OK;
    }

    if(!ip2region_create(&g_ip2region_entry, (char *)conf->db_file.data)) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "ip2region create failed: %V", &conf->db_file);
        return NGX_ERROR;
    }

    switch(conf->algo) {
        default:
        case NGX_IP2REGION_MEMORY:
            ip2region_search_func = ip2region_memory_search;
            break;

        case NGX_IP2REGION_BINARY:
            ip2region_search_func = ip2region_binary_search;
            break;

        case NGX_IP2REGION_BTREE:
            ip2region_search_func = ip2region_btree_search;
            break;
    }

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "ip2region create succcess: algo=%ui", conf->algo);
    return NGX_OK;
}

static void
ngx_ip2region_exit_process(ngx_cycle_t *cycle)
{
    ip2region_destroy(&g_ip2region_entry);
    ngx_log_debug0(NGX_LOG_INFO, ngx_cycle->log, 0, "ip2region destroy");
}

static ngx_inline u_char *
ngx_strrchr(u_char *first, u_char *p, u_char country_name)
{
    while(p >= first) {
        if(*p == country_name) {
            return p;
        }

        p--;
    }

    return NULL;
}

static ngx_inline uint32_t
ngx_inet_aton(u_char *data, ngx_uint_t len)
{
    long long  result = 0;
    long long  temp = 0;
    u_char      *last = data + len;
    int        point = 0;

    while(data <= last) {
        if(*data == '.' || data == last) {
            if(temp > 255) {
                return NGX_ERROR;
            }

            result = (result << 8) + temp;
            temp = 0;
            ++point;

            if(point == 4) {
                break;
            }

        } else if(*data <= '9' && *data >= '0') {
            temp = temp * 10 + (*data - '0');

        } else {
            return NGX_ERROR;
        }

        ++data;
    }

    if(point != 4) {
        return NGX_ERROR;
    }

    return result;
}

ngx_int_t
ngx_ip2region_search(ngx_str_t *addr_text, ngx_str_t *isp, ngx_str_t *city)
{
    if(g_ip2region_entry.dbHandler == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "search error ip %V, ip2region doesn't create", addr_text);
        return NGX_ERROR;
    }

    uint32_t addr_binary = ngx_inet_aton(addr_text->data, addr_text->len);

    if(addr_binary == (uint32_t)NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "search error ip %V: bad ip address", addr_text);
        return NGX_ERROR;
    }

    datablock_entry datablock;
    ngx_memzero(&datablock, sizeof(datablock));
    uint_t rc = ip2region_search_func(&g_ip2region_entry, (uint_t)addr_binary, &datablock);

    if(!rc) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "%V search failed", addr_text);
        return NGX_ERROR;
    }

    ngx_log_debug2(NGX_LOG_INFO, ngx_cycle->log, 0, "%V: %s", addr_text, datablock.region);
    //城市Id 国家|区域|省份|城市|ISP
    u_char *end = (u_char *)(datablock.region + ngx_strlen(datablock.region));
    u_char *pos = ngx_strrchr((u_char *)datablock.region, end, '|');

    if(!pos) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "search isp failed");
        return NGX_ERROR;
    }

    u_char isp_buffer[64] = {0};
    ngx_snprintf(isp_buffer, sizeof(isp_buffer), "%*s%Z", end - pos, pos + 1);
    ngx_log_debug3(NGX_LOG_INFO, ngx_cycle->log, 0, "%V: %s [%s]", addr_text, datablock.region, (char *)isp_buffer);

    if(city) {
        end = pos - 1;
        u_char *pos_city = ngx_strrchr((u_char *)datablock.region, end, '|');

        if(!pos_city) {
            ngx_log_debug0(NGX_LOG_ERR, ngx_cycle->log, 0, "search city failed");
            return NGX_ERROR;
        }

        u_char city_buffer[64] = {0};
        ngx_snprintf(city_buffer, sizeof(city_buffer), "%*s%Z", end - pos_city, pos_city + 1);
    }

    return NGX_OK;
}

/**
 * 要添加的变量 数组
 */
static ngx_http_variable_t  ngx_http_ip2region_vars[] = {

        { ngx_string("ip2region_region_name"), NULL,
          ngx_http_ip2region_region_name,
          0, 0, 0 },

        { ngx_string("ip2region_country_name"), NULL,
          ngx_http_ip2region_country_name,
          1, 0, 0 },

        { ngx_null_string, NULL, NULL, 0, 0, 0 }
};

/**
 * 设置第一个变量，只有这个变量查ip2region库，获取region
 * 后面其他变量从这个变量中拿到region解析出来
 */
static ngx_int_t
ngx_http_ip2region_region_name(ngx_http_request_t *r,
                        ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_str_t              *addr_text;

    datablock_entry *datablock;
    datablock = ngx_ip2region_get_datablock(addr_text);
    if (!datablock) {//datablock 是 NULL
        return NGX_ERROR;
    }

    v->len = sizeof(datablock->region);
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->data = &datablock->region;

    return NGX_OK;
}

static ngx_int_t
ngx_http_ip2region_country_name(ngx_http_request_t *r,
                        ngx_http_variable_value_t *v, uintptr_t data)
{

//    ngx_ip2region_conf_t  *ctx;
//    ctx = ngx_http_get_module_ctx(r, ngx_ip2region_module);

//
//    ngx_int_t index = ngx_http_get_variable_index(ngx_conf_t *cf, ngx_str_t *name);
//    ngx_http_variable_value_t *vvv;
//    vvv = ngx_http_get_indexed_variable(r, index);


    char[] country_name = "country_name";

    v->len = sizeof(country_name);
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->data = &country_name;


    return NGX_OK;
}

datablock_entry *
ngx_ip2region_get_datablock(ngx_str_t *addr_text)
{
//    ngx_str_t *isp;
//    ngx_str_t *city;
    //判断ip2region是否实例化了
    if(g_ip2region_entry.dbHandler == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "search error ip %V, ip2region doesn't create", addr_text);
        return NULL;
    }
    //获取二进制地址
    uint32_t addr_binary = ngx_inet_aton(addr_text->data, addr_text->len);
    //ip地址格式错误
    if(addr_binary == (uint32_t)NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "search error ip %V: bad ip address", addr_text);
        return NULL;
    }

    datablock_entry datablock;
    //分配内存
    ngx_memzero(&datablock, sizeof(datablock));
    //查库
    uint_t rc = ip2region_search_func(&g_ip2region_entry, (uint_t)addr_binary, &datablock);

    if(!rc) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "%V search failed", addr_text);
        return NULL;
    }

//    ngx_log_debug2(NGX_LOG_INFO, ngx_cycle->log, 0, "%V: %s", addr_text, datablock.region);
    return datablock;
}


/**
 * 遍历ngx_http_ip2region_vars数组，逐个添加变量
 * 变量的值取决于get_handler方法
 */
static ngx_int_t
ngx_http_ipdb_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_ip2region_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}

/*
static ngx_int_t
ngx_http_ipdb_add_variables11111111(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;
    var = ngx_http_add_variable(cf, &v->name, v->flags);

    var = ngx_http_get_variable_index(cf, &v->name);

    ngx_int_t ngx_http_get_variable_index(ngx_conf_t *cf, ngx_str_t *name);
    ngx_http_variable_value_t *ngx_http_get_indexed_variable(ngx_http_request_t *r,
                                                             ngx_uint_t index);
    ngx_http_variable_value_t *ngx_http_get_flushed_variable(ngx_http_request_t *r,
                                                             ngx_uint_t index);
    return NGX_OK;
}*/
