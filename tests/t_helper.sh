#!/bin/bash
# Unprivileged test of maxeco-helper against a fake sysfs tree.
set -u
HELPER=${1:-build/maxeco-helper}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export MAXECO_SYSROOT=$T/root MAXECO_RUNDIR=$T/run MAXECO_NO_NVML=1
C=$T/root/sys/devices/system/cpu/cpufreq
fail=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got '$2' want '$3'"; fail=1; fi; }
mk() { mkdir -p "$(dirname "$1")"; printf '%s\n' "$2" > "$1"; }
for i in $(seq 0 15); do
  P=$C/policy$i
  mk $P/scaling_available_governors "performance powersave"
  mk $P/scaling_governor performance
  mk $P/energy_performance_preference performance
  mk $P/cpuinfo_min_freq 603379
  mk $P/cpuinfo_max_freq 5271622
  mk $P/scaling_max_freq 5271622
done
mk $C/policy5/scaling_max_freq 2000000   # the user already capped this one below Eco
mk $C/boost 1
D=$T/root/sys/class/drm
# integrated AMD GPU, no screen, no power cap
mk $D/card1/device/vendor 0x1002
mk $D/card1/device/power_dpm_force_performance_level auto
mk $D/card1-DP-1/status disconnected
mk $D/card2/device/vendor 0x10de
# AMD card driving a screen, with a power cap (microwatts)
mk $D/card3/device/vendor 0x1002
mk $D/card3/device/power_dpm_force_performance_level auto
A=$D/card3/device/hwmon/hwmon9
mk $A/power1_cap 200000000
mk $A/power1_cap_default 220000000
mk $A/power1_cap_min 100000000
mk $A/power1_cap_max 250000000
mk $D/card3-HDMI-A-1/status connected
# motherboard fan chip: fans on channels 2 and 4, nothing on 1, CPU on temp7
H=$T/root/sys/class/hwmon
mk $H/hwmon1/name k10temp
mk $H/hwmon3/name nct6799
mk $H/hwmon3/temp1_label SYSTIN
mk $H/hwmon3/temp7_label "SMBUSMASTER 0"
for c in 1 2 4; do
  mk $H/hwmon3/pwm$c 255
  mk $H/hwmon3/pwm${c}_enable 0
  mk $H/hwmon3/pwm${c}_temp_sel 1
  for k in 1 2 3 4 5; do mk $H/hwmon3/pwm${c}_auto_point${k}_temp $((k * 10000)); mk $H/hwmon3/pwm${c}_auto_point${k}_pwm $((k * 50)); done
done
mk $H/hwmon3/fan1_input 0
mk $H/hwmon3/fan2_input 1544
mk $H/hwmon3/fan4_input 1493
# another brand of chip (no curve support here): never touched
mk $H/hwmon4/name it8689
mk $H/hwmon4/pwm1 128
mk $H/hwmon4/pwm1_enable 1
mk $H/hwmon4/fan1_input 900
# an nct chip without a processor input: left alone
mk $H/hwmon5/name nct6798
mk $H/hwmon5/pwm1 255
mk $H/hwmon5/pwm1_enable 0
mk $H/hwmon5/fan1_input 800
lvl() { cat $T/run/level 2>/dev/null || echo none; }
points() { for k in 1 2 3 4 5; do printf '%s/%s ' $(cat $H/hwmon3/pwm$1_auto_point${k}_temp) $(cat $H/hwmon3/pwm$1_auto_point${k}_pwm); done; }
CURVE="25000/89 55000/89 70000/153 80000/255 90000/255 "

check "status while off" "$($HELPER status)" "level: Normal"
$HELPER resume; check "resume while off does nothing" "$(cat $C/policy0/scaling_governor) $(lvl)" "performance none"
$HELPER level bogus 2>/dev/null; check "unknown level refused" "$? $(test -e $T/run/state && echo saved || echo nothing)" "2 nothing"

$HELPER save > $T/save.log
check "save changes nothing" "$(cat $C/policy7/scaling_governor) $(cat $H/hwmon3/pwm2_enable) $(lvl)" "performance 0 none"
check "state lines (16 gov + 16 epp + 16 max + 2 amd levels + 1 amd cap + 2 fans x 14)" "$(grep -c "^file" $T/run/state)" 79
check "state mode" "$(stat -c %a $T/run/state)" 644
mk $H/hwmon3/fan1_input 700   # a fan that starts after the save is not in the state

echo "--- Eco"
$HELPER level eco > $T/eco.log
check "governor" "$(cat $C/policy7/scaling_governor)" powersave
check "epp" "$(cat $C/policy7/energy_performance_preference)" balance_power
check "max freq 60 %" "$(cat $C/policy15/scaling_max_freq)" 3162973
check "a lower cap set by the user is kept" "$(cat $C/policy5/scaling_max_freq)" 2000000
check "boost left alone" "$(cat $C/boost)" 1
check "unused AMD GPU low" "$(cat $D/card1/device/power_dpm_force_performance_level)" low
check "AMD GPU with a screen keeps its level" "$(cat $D/card3/device/power_dpm_force_performance_level)" auto
check "AMD power cap 60 % of default" "$(cat $A/power1_cap)" 132000000
check "fan curve mode" "$(cat $H/hwmon3/pwm2_enable) $(cat $H/hwmon3/pwm4_enable)" "5 5"
check "fan curve follows the CPU" "$(cat $H/hwmon3/pwm2_temp_sel)" 7
check "fan curve points" "$(points 4)" "$CURVE"
check "fan not in the saved state untouched" "$(cat $H/hwmon3/pwm1_enable) $(cat $H/hwmon3/pwm1_auto_point1_pwm)" "0 50"
check "other chip untouched" "$(cat $H/hwmon4/pwm1_enable) $(cat $H/hwmon4/pwm1)" "1 128"
check "chip without CPU input untouched" "$(cat $H/hwmon5/pwm1_enable)" 0
check "level file" "$(lvl) $(stat -c %a $T/run/level)" "eco 644"
check "status" "$($HELPER status | head -1)" "level: Eco"
$HELPER save > /dev/null
check "a second save keeps the original values" "$(grep -c 'scaling_governor	performance' $T/run/state)" 16

echo "--- MaxEco"
mk $H/hwmon3/pwm2 120   # what the chip shows while it runs the curve
$HELPER level max > $T/max.log
check "max freq lowest" "$(cat $C/policy15/scaling_max_freq) $(cat $C/policy5/scaling_max_freq)" "603379 603379"
check "epp power" "$(cat $C/policy7/energy_performance_preference)" power
check "AMD GPU with a screen low" "$(cat $D/card3/device/power_dpm_force_performance_level)" low
check "AMD power cap" "$(cat $A/power1_cap)" 132000000
check "fan curve already running is not rewritten" "$(cat $H/hwmon3/pwm2) $(cat $H/hwmon3/pwm2_enable)" "120 5"
check "level file" "$(lvl)" max

echo "--- back to Eco"
$HELPER level eco > /dev/null
check "Eco values again" "$(cat $C/policy15/scaling_max_freq) $(cat $C/policy7/energy_performance_preference)" "3162973 balance_power"
check "AMD level back to the original" "$(cat $D/card3/device/power_dpm_force_performance_level)" auto
check "user cap back" "$(cat $C/policy5/scaling_max_freq)" 2000000

echo "--- resume"
mk $C/policy3/scaling_max_freq 5271622   # forgotten during a suspend
mk $H/hwmon3/pwm4_enable 1               # another tool set the fan to manual
$HELPER resume > $T/resume.log
check "resume applies the current level" "$(cat $C/policy3/scaling_max_freq) $(cat $H/hwmon3/pwm4_enable) $(lvl)" "3162973 5 eco"
check "resume rewrites the whole fan curve" "$(points 4)" "$CURVE"

# tampering: a path outside the tree must be ignored on restore
echo "victim" > $T/victim
printf 'file\t%s\tpwned\n' "$T/victim" >> $T/run/state
printf 'file\t%s/sys/../../victim\tpwned\n' "$MAXECO_SYSROOT" >> $T/run/state
echo "--- off"
$HELPER off > $T/off.log
check "governor restored" "$(cat $C/policy7/scaling_governor)" performance
check "epp restored" "$(cat $C/policy7/energy_performance_preference)" performance
check "max restored" "$(cat $C/policy15/scaling_max_freq) $(cat $C/policy5/scaling_max_freq)" "5271622 2000000"
check "amdgpu restored" "$(cat $D/card1/device/power_dpm_force_performance_level) $(cat $D/card3/device/power_dpm_force_performance_level)" "auto auto"
check "AMD power cap restored" "$(cat $A/power1_cap)" 200000000
check "fan mode restored" "$(cat $H/hwmon3/pwm2_enable) $(cat $H/hwmon3/pwm4_enable)" "0 0"
check "fan input restored" "$(cat $H/hwmon3/pwm2_temp_sel)" 1
check "fan points restored" "$(cat $H/hwmon3/pwm2_auto_point1_temp)/$(cat $H/hwmon3/pwm2_auto_point1_pwm) $(cat $H/hwmon3/pwm2_auto_point5_temp)/$(cat $H/hwmon3/pwm2_auto_point5_pwm)" "10000/50 50000/250"
check "outside path untouched" "$(cat $T/victim)" victim
check "state and level removed" "$(test -e $T/run/state && echo yes || echo no) $(lvl)" "no none"
check "off again is harmless" "$($HELPER off)" "MaxEco is not on, nothing to restore"
check "nvidia card untouched" "$(ls $D/card2/device)" vendor

echo "--- MaxEco 1.1 command"
$HELPER on > /dev/null
check "on = MaxEco, saving first" "$(cat $C/policy0/scaling_max_freq) $(lvl) $(grep -c 'scaling_governor	performance' $T/run/state)" "603379 max 16"
rm $T/run/level   # state saved by MaxEco 1.1 has no level file
check "no level file counts as MaxEco" "$($HELPER status | head -1)" "level: MaxEco"
$HELPER off > /dev/null
check "and off restores it" "$(cat $C/policy0/scaling_max_freq) $(cat $C/policy0/scaling_governor)" "5271622 performance"

cat $T/save.log $T/eco.log $T/max.log $T/resume.log $T/off.log
exit $fail
