#!/usr/bin/env python3
"""Patch IDF's esp_pm/pm_impl.c with sleep-time instrumentation.

Usage: patch_pm_impl.py <input pm_impl.c> <output pm_impl_patched.c>
Adds: g_pm_sleep_us, g_pm_sleep_count (sleep time + entries) and
g_pm_skip_locks / g_pm_skip_short (skip-reason counters).
"""
import sys

src = open(sys.argv[1]).read()

anchor = 'static portMUX_TYPE s_switch_lock = portMUX_INITIALIZER_UNLOCKED;'
src = src.replace(anchor, anchor + '''

/* ChonkCore instrumentation: cumulative light sleep time, entries, and
   skip-reason counters. */
uint64_t g_pm_sleep_us = 0;
uint32_t g_pm_sleep_count = 0;
uint32_t g_pm_skip_locks = 0;
uint32_t g_pm_skip_short = 0;''', 1)

old = '''    if (s_skip_light_sleep[core_id]) {
        s_skip_light_sleep[core_id] = false;
        s_skipped_light_sleep[core_id] = true;
        return true;
    }'''
src = src.replace(old, '''    if (s_skip_light_sleep[core_id]) {
        s_skip_light_sleep[core_id] = false;
        s_skipped_light_sleep[core_id] = true;
        g_pm_skip_locks++;
        return true;
    }''', 1)

old = '''    return s_skipped_light_sleep[core_id];
}'''
src = src.replace(old, '''    if (s_skipped_light_sleep[core_id]) g_pm_skip_locks++;
    return s_skipped_light_sleep[core_id];
}''', 1)

old = '''        if (sleep_time_us >= configEXPECTED_IDLE_TIME_BEFORE_SLEEP * portTICK_PERIOD_MS * 1000LL) {'''
src = src.replace(old, '''        if (sleep_time_us < configEXPECTED_IDLE_TIME_BEFORE_SLEEP * portTICK_PERIOD_MS * 1000LL) {
            g_pm_skip_short++;
        }
        if (sleep_time_us >= configEXPECTED_IDLE_TIME_BEFORE_SLEEP * portTICK_PERIOD_MS * 1000LL) {''', 1)

old = '''            int64_t slept_us = esp_timer_get_time() - sleep_start;
            ESP_PM_TRACE_EXIT(SLEEP, core_id);'''
src = src.replace(old, '''            int64_t slept_us = esp_timer_get_time() - sleep_start;
            ESP_PM_TRACE_EXIT(SLEEP, core_id);
            g_pm_sleep_us += slept_us;
            g_pm_sleep_count++;''', 1)

open(sys.argv[2], 'w').write(src)
print("patched", sys.argv[2])
