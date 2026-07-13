/*
 * TFLite Micro person detection wrapper.
 * Model: MobileNet-based 96x96 grayscale int8 quantized (from esp-tflite-micro examples).
 * Input:  YUYV frame, nearest-neighbour downscaled to 96x96, Y-channel only.
 * Output: [no_person, person] int8 scores → 0-100 confidence.
 */
#include "person_detect.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "person_detect_model_data.h"   /* g_person_detect_model_data[] from managed_components */

#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>

static const char *TAG = "person_detect";

static constexpr int kInW         = 96;
static constexpr int kInH         = 96;
static constexpr int kArenaSRAM   = 120;  /* used=119KB measured; try internal SRAM first (fast) */
static constexpr int kArenaPSRAM  = 200;  /* PSRAM fallback (10-20x slower) */
static constexpr int kPersonIdx   = 1;    /* output index: person score */

static uint8_t *s_arena    = nullptr;
static tflite::MicroInterpreter         *s_interp   = nullptr;
static tflite::MicroMutableOpResolver<5> s_resolver;

bool person_detect_init(void)
{
    if (s_interp) { return true; }

    /* Internal SRAM: 10-20x faster inference than PSRAM due to cache/bandwidth.
     * Person detect model needs ~120-140 KB arena. Fall back to PSRAM if unavailable. */
    int arena_kb = kArenaSRAM;
    s_arena = (uint8_t *)heap_caps_malloc((size_t)kArenaSRAM * 1024,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_arena) {
        ESP_LOGW(TAG, "Internal SRAM arena (%dKB) unavailable, falling back to PSRAM (slow)",
                 kArenaSRAM);
        arena_kb = kArenaPSRAM;
        s_arena = (uint8_t *)heap_caps_malloc((size_t)kArenaPSRAM * 1024, MALLOC_CAP_SPIRAM);
    }
    if (!s_arena) {
        ESP_LOGE(TAG, "Arena alloc failed (SRAM %dKB, PSRAM %dKB)", kArenaSRAM, kArenaPSRAM);
        return false;
    }
    ESP_LOGI(TAG, "Arena: %dKB in %s", arena_kb,
             (arena_kb == kArenaSRAM) ? "internal SRAM" : "PSRAM");

    const tflite::Model *model = tflite::GetModel(g_person_detect_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema %lu != runtime %d",
                 (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        heap_caps_free(s_arena); s_arena = nullptr;
        return false;
    }

    s_resolver.AddConv2D();
    s_resolver.AddDepthwiseConv2D();
    s_resolver.AddAveragePool2D();
    s_resolver.AddReshape();
    s_resolver.AddSoftmax();

    static tflite::MicroInterpreter static_interp(
        model, s_resolver, s_arena, (size_t)arena_kb * 1024);
    s_interp = &static_interp;

    if (s_interp->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        s_interp = nullptr;
        heap_caps_free(s_arena); s_arena = nullptr;
        return false;
    }

    TfLiteTensor *in = s_interp->input(0);
    ESP_LOGI(TAG, "Init ok — input [%d,%d,%d,%d] type=%d zp=%d scale=%.6f arena=%dKB used=%zuKB",
             in->dims->data[0], in->dims->data[1],
             in->dims->data[2], in->dims->data[3],
             (int)in->type,
             in->params.zero_point, (double)in->params.scale,
             arena_kb,
             s_interp->arena_used_bytes() / 1024);
    return true;
}

int person_detect_run(const uint8_t *yuyv, int width, int height)
{
    if (!s_interp || !yuyv) { return -1; }

    TfLiteTensor *input = s_interp->input(0);
    int8_t *in_data = input->data.int8;

    /* Nearest-neighbour downsample YUYV → 96×96 grayscale int8.
     * YUYV packing: [Y0][U][Y1][V] per 2 pixels → Y at byte (y*width+x)*2. */
    const float scale     = input->params.scale > 0.0f ? input->params.scale : (1.0f / 127.5f);
    const int   zero_pt   = input->params.zero_point;
    for (int oy = 0; oy < kInH; oy++) {
        int sy = oy * height / kInH;
        const uint8_t *row = yuyv + (size_t)sy * width * 2;
        int8_t *dst = in_data + oy * kInW;
        for (int ox = 0; ox < kInW; ox++) {
            int sx = ox * width / kInW;
            uint8_t y_byte = row[sx * 2];
            /* Model expects float [-1.0, 1.0]: pixel/127.5 - 1.0 */
            float   f_val  = (float)y_byte / 127.5f - 1.0f;
            int     q_val  = (int)roundf(f_val / scale) + zero_pt;
            if (q_val > 127)  q_val = 127;
            if (q_val < -128) q_val = -128;
            dst[ox] = (int8_t)q_val;
        }
    }

    if (s_interp->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed");
        return -1;
    }

    TfLiteTensor *output = s_interp->output(0);
    /* Output: [no_person, person] int8 → dequantize to 0-255 then to 0-100 */
    int8_t raw_no  = output->data.int8[0];
    int8_t raw_yes = output->data.int8[kPersonIdx];
    int no_p  = (int)raw_no  + 128;  /* shift to 0-255 */
    int yes_p = (int)raw_yes + 128;
    int score = yes_p * 100 / 255;

    ESP_LOGI(TAG, "Score: no_person=%d person=%d → %d%%", no_p, yes_p, score);
    return score;
}

void person_detect_deinit(void)
{
    s_interp = nullptr;
    if (s_arena) { heap_caps_free(s_arena); s_arena = nullptr; }
}
