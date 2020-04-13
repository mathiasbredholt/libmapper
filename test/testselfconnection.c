#include <stdio.h>
#include "mpr/mpr.h"

void handler(mpr_sig sig, mpr_sig_evt evt, mpr_id inst, int length,
             mpr_type type, const void *value, mpr_time time) {
  const char *name = mpr_obj_get_prop_as_str(sig, MPR_PROP_NAME, NULL);
  printf("%s set to: %d\n", name, *((int *)value));
}

int main() {
  mpr_dev dev = mpr_dev_new("test", 0);
  mpr_sig input1 = mpr_sig_new(dev, MPR_DIR_IN, "input1", 1, MPR_INT32, 0, 0, 0,
                               0, handler, MPR_SIG_UPDATE);
  mpr_sig output1 = mpr_sig_new(dev, MPR_DIR_OUT, "output1", 1, MPR_INT32, 0, 0,
                                0, 0, handler, MPR_SIG_UPDATE);
  mpr_sig input2 = mpr_sig_new(dev, MPR_DIR_IN, "input2", 1, MPR_INT32, 0, 0, 0,
                               0, handler, MPR_SIG_UPDATE);
  mpr_sig output2 = mpr_sig_new(dev, MPR_DIR_OUT, "output2", 1, MPR_INT32, 0, 0,
                                0, 0, handler, MPR_SIG_UPDATE);
  while (!mpr_dev_get_is_ready(dev)) {
    mpr_dev_poll(dev, 25);
  }
  mpr_obj_push(mpr_map_new(1, &output1, 1, &input1));
  mpr_obj_push(mpr_map_new(1, &output2, 1, &input2));
  int i = 0;
  while (1) {
    int j = i * 2;
    mpr_dev_poll(dev, 100);
    mpr_sig_set_value(output1, 0, 1, MPR_INT32, &i, MPR_NOW);
    mpr_sig_set_value(output2, 0, 1, MPR_INT32, &j, MPR_NOW);
    ++i;
  }
}
