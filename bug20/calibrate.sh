#!/system/bin/sh
# TouchScreen calibration helper

# poor man's if [ ! -f
/system/bin/cat /data/system/tslib/pointercal > /dev/null 2> /dev/null ||
  /system/bin/ts_calibrate nofork
