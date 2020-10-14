#include "init_module.h"
#include "plugins.h"
#include "ntp_enet.h"

#define DBG_TAG "ntp"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define NTP_SERVER          "cn.pool.ntp.org"
#define NTP_TTL             4

static struct plugins_module ntp_plugin = {
	.name = "ntp",
	.version = "v1.0.0",
	.author = "malongwei"
};

static struct init_module ntp_init_module;

static void ntp_sync_entry(void *parameter)
{
/* NTP校时错误重试次数 */
#define MAX_ERROR_CNT   3
/* NTP校时错误重试间隔(s) */
#define ERROR_INTERVAL  20

    ntp_plugin.state = PLUGINS_STATE_RUNNING;

    struct tm info;
    uint8_t error_cnt = 0;
    rt_thread_mdelay(5000);
    while(1)
    {
        int rc = -RT_ERROR;

        time_t ntp_time = ntp_get_time(NTP_SERVER);
        if (ntp_time > 0)
        {
            rc = RT_EOK;
            localtime_r(&ntp_time, &info);
            info.tm_year += 1900;
            info.tm_mon += 1;
        }

        if(rc != RT_EOK)
        {
            error_cnt++;
        }
        else
        {
            LOG_I("Sync Success. Info: %d-%d-%d %d:%d:%d", info.tm_year, info.tm_mon, info.tm_mday,
                                                           info.tm_hour, info.tm_min, info.tm_sec);
            set_date(info.tm_year, info.tm_mon, info.tm_mday);
            set_time(info.tm_hour, info.tm_min, info.tm_sec);
        }

        if((error_cnt >= MAX_ERROR_CNT) || (rc == RT_EOK))
        {
            error_cnt = 0;
            int ntp_ttl = NTP_TTL;
            while(ntp_ttl > 0)
            {
                rt_thread_mdelay(3600 * 1000);
                ntp_ttl--;
            }
        }
        else
        {
            LOG_W("Sync failed.");
            rt_thread_mdelay(ERROR_INTERVAL * 1000);
        }

        rt_thread_mdelay(10);
    }
}

static int ntp_init(void)
{
    rt_thread_t tid = rt_thread_create("ntp_sync", ntp_sync_entry, RT_NULL, 1600, 21, 100);
    if(tid != RT_NULL)
        rt_thread_startup(tid);

    return RT_EOK;
}

int fregister(const char *path, void *dlmodule, uint8_t is_sys)
{
    plugins_register(&ntp_plugin, path, dlmodule, is_sys);

    ntp_init_module.init = ntp_init;
    init_module_app_register(&ntp_init_module);

    return RT_EOK;
}
