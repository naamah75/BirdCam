#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int bc_get_framesize();
int bc_get_jpeg_quality();
int bc_get_img_mode();

void bc_apply_settings(int framesize, int jpeg_quality, int img_mode);
void bc_save_settings();

int bc_get_archive_keep();
int bc_set_archive_keep(int keep);

int bc_get_snapshot_count();
uint32_t bc_get_snapshot_bytes_used();
uint32_t bc_get_snapshot_bytes_limit();


// Camera image controls (persisted)
int bc_get_brightness();      // -2..2
int bc_get_contrast();        // -2..2
int bc_get_saturation();      // -2..2
int bc_get_sharpness();       // -2..2 (if supported)
int bc_get_gain_ctrl();       // 0/1 auto gain
int bc_get_exposure_ctrl();   // 0/1 auto exposure
int bc_get_awb();             // 0/1 auto white balance
int bc_get_agc_gain();        // 0..30 (manual gain)
int bc_get_aec_value();       // 0..1200 (manual exposure)

void bc_apply_cam_controls(int brightness, int contrast, int saturation, int sharpness,
                           int gain_ctrl, int exposure_ctrl, int awb,
                           int agc_gain, int aec_value);


#ifdef __cplusplus
}
#endif
