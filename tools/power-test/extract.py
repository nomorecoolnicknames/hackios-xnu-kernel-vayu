#!/usr/bin/env python3
"""Host includes made of the kernel tree's own functions (no copies of their logic): extract.py TREE BUILD."""
import pathlib
import re
import sys

tree, build = map(pathlib.Path, sys.argv[1:])


def source(path):
    return (tree / path).read_text()


def functions(text, *names):
    out = []
    for name in names:
        match = re.search(r"^(?:static )?(?:inline )?[^\n;{}]+\n" + re.escape(name) +
                          r"\([^\n]*\)\n\{.*?^\}", text, re.M | re.S)
        if not match:
            raise ValueError(name)
        out.append(match.group(0))
    return "\n\n".join(out) + "\n"


def block(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]


pas = source("bsd/dev/vayu_pas.c")
header = source("bsd/sys/vayu_pas.h")
parts = [block(pas, "#define SMEM_PHYS", "static const volatile uint8_t *smem;"),
         block(header, "#define VAYU_GLINK_CHANNELS", "bool vayu_glink_prepare"),
         block(pas, "#define GLINK_OUR_TAIL", "static void\nglink_lock_ready"),
         "static const volatile uint8_t *smem;\n",
         functions(pas, "vayu_smem_peek", "vayu_smem_word", "vayu_smem_poke", "smem_partition",
                   "smem_find", "smem_item", "vayu_smem_item", "glink_bad", "glink_theirs_queue",
                   "glink_theirs", "glink_waiting", "glink_peek", "glink_advance"),
         '#include "pas_stubs.inc"\n', functions(pas, "glink_pump"),
         block(header, "enum vayu_rsc_tcs_result {", "enum vayu_rsc_tcs_result vayu_rsc_tcs_lock"),
         block(pas, "#define RPMH_TCS_WARN_US", "/*\n * TCS 0 of the apps RSC"),
         '#include "rpmh_stubs.inc"\n', functions(pas, "vayu_rsc_tcs_lock", "vayu_rsc_tcs_unlock")]
(build / "pas.inc").write_text("\n".join(parts))

cpu = source("bsd/dev/vayu_cpufreq.c")
parts = [block(cpu, "#define OSM_DOMAINS", "/*\n * The system cache's own"),
         block(cpu, "#define OSM_L3_PHYS", "static void\ntell"),
         block(cpu, "struct osm_row {", "static void\nread_row"),
         block(cpu, "struct raised_domain {", "/*\n * The rows the thermal throttle"),
         block(cpu, "#define TSENS_CONTROLLERS", "static const uint32_t tsens_tm_phys"),
         block(cpu, "static const char *const tsens_name", "static const uint16_t pmic_alarm_id"),
         block(cpu, "static const char *const pmic_name", "/*\n * cpufreq_gov=1"),
         block(cpu, "#define GOVERNOR_BUSY_TOP", "/* The row the group should be at"),
         block(cpu, "static struct {\n\tvm_offset_t base;\n\tuint32_t was, top;\n\tuint32_t rest;", "static void\nraise_l3"),
         functions(cpu, "read_row", "top_row", "thermal_rows", "build_ladder", "raise_domain", "raise_l3",
                   "effective_level", "wanted_row", "applied_signature", "now_ns", "thermal_apply", "thermal_thread",
                   "vayu_cpufreq_quiesce")]
# The host may be x86; the MMIO ordering barrier has no hardware in this test.
(build / "thermal.inc").write_text("\n".join(parts).replace(
    '__asm__ volatile ("dsb sy" ::: "memory");', '__atomic_thread_fence(__ATOMIC_SEQ_CST);'))

battery = source("bsd/dev/hakios.battery.c")
torch = source("bsd/dev/hakios.torch.c")
buttons = source("bsd/dev/hakios.buttons.c")
torch_macros = ("FLASH_INT_RT_STS", "V2_STATUS1", "V2_STATUS2", "V2_STATUS2_OTST", "V2_STATUS2_VPH_DROOP",
                "STATUS2_OTST1", "STATUS2_OTST2", "STATUS2_VPH_LOW", "ALARM_STATUS", "RT_STS_FAULT", "HOTTEST_BATTERY_C")
macros = "\n".join(re.search(r"^#define\s+" + name + r"\s+[^\n]+", torch, re.M).group(0)
                   for name in torch_macros)
parts = [macros, block(battery, "#define BATTERY_TEMPERATURE_MAX_AGE_NS", "static void\nbattery_temperature_update"),
         functions(battery, "battery_temperature_update", "battery_loader_temperature_update", "hakios_battery_temperature"),
         functions(torch, "off_bits", "v2_dark", "trouble"), functions(buttons, "hakios_spmi_peek"),
         functions(source("bsd/dev/hakios.cpufreq.c"), "cpufreq_wanted")]
(build / "guards.inc").write_text("\n".join(parts))
