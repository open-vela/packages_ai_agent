/* SPDX-License-Identifier: Apache-2.0 */

#include "tools/mooncat_local_router.h"
#include "tools/mooncat_intent_model_data.h"

#include <cmath>
#include <cstdint>
#include <new>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

constexpr int kSuccess = 0;
constexpr int kFailure = -1;

static_assert(MOONCAT_INTENT_MODEL_FEATURE_COUNT ==
                  MOONCAT_ROUTER_FEATURE_COUNT,
              "router and model feature dimensions must match");
static_assert(MOONCAT_INTENT_COACH_NOW == 0 &&
                  MOONCAT_INTENT_COACH_PREVIEW == 1 &&
                  MOONCAT_INTENT_COACH_SCHEDULE == 2 &&
                  MOONCAT_INTENT_COACH_LIST == 3 &&
                  MOONCAT_INTENT_FALLBACK == 4,
              "router intent order must match generated model labels");
static_assert(MOONCAT_INTENT_MODEL_LABEL_COUNT ==
                  MOONCAT_INTENT_FALLBACK + 1,
              "router and model label counts must match");

constexpr size_t kTensorArenaBytes = 32 * 1024;
alignas(16) uint8_t g_tensor_arena[kTensorArenaBytes];
uint8_t g_features[MOONCAT_INTENT_MODEL_FEATURE_COUNT];
tflite::MicroInterpreter *g_interpreter;

int quantized_value(float value, const TfLiteTensor *tensor)
{
    int result = static_cast<int>(
        std::lround(value / tensor->params.scale)) + tensor->params.zero_point;
    if (result < -128) {
        return -128;
    }
    if (result > 127) {
        return 127;
    }
    return result;
}

float dequantized_value(int8_t value, const TfLiteTensor *tensor)
{
    return (static_cast<int>(value) - tensor->params.zero_point) *
           tensor->params.scale;
}

int initialize_interpreter()
{
    if (g_interpreter) {
        return kSuccess;
    }

    const tflite::Model *model = tflite::GetModel(g_mooncat_router_model);
    if (!model || model->version() != TFLITE_SCHEMA_VERSION) {
        return kFailure;
    }

    static tflite::MicroMutableOpResolver<2> resolver;
    static bool resolver_ready;
    if (!resolver_ready) {
        if (resolver.AddFullyConnected(tflite::Register_FULLY_CONNECTED_INT8()) !=
                kTfLiteOk ||
            resolver.AddSoftmax(tflite::Register_SOFTMAX_INT8()) != kTfLiteOk) {
            return kFailure;
        }
        resolver_ready = true;
    }

    g_interpreter = new (std::nothrow) tflite::MicroInterpreter(
        model, resolver, g_tensor_arena, sizeof(g_tensor_arena));
    if (!g_interpreter || g_interpreter->AllocateTensors() != kTfLiteOk) {
        delete g_interpreter;
        g_interpreter = nullptr;
        return kFailure;
    }

    TfLiteTensor *input = g_interpreter->input(0);
    TfLiteTensor *output = g_interpreter->output(0);
    if (!input || !output || input->type != kTfLiteInt8 ||
        output->type != kTfLiteInt8 ||
        input->bytes != MOONCAT_INTENT_MODEL_FEATURE_COUNT ||
        output->bytes != MOONCAT_INTENT_MODEL_LABEL_COUNT ||
        input->params.scale <= 0.0f || output->params.scale <= 0.0f) {
        delete g_interpreter;
        g_interpreter = nullptr;
        return kFailure;
    }
    return kSuccess;
}

}  // namespace

extern "C" int mooncat_intent_model_predict(
    const char *text, struct mooncat_router_prediction_s *prediction)
{
    if (!text || !prediction || initialize_interpreter() != kSuccess) {
        return kFailure;
    }

    TfLiteTensor *input = g_interpreter->input(0);
    mooncat_router_featurize(text, g_features, sizeof(g_features));
    const int zero = quantized_value(0.0f, input);
    const int one = quantized_value(1.0f, input);
    for (size_t i = 0; i < sizeof(g_features); i++) {
        input->data.int8[i] = static_cast<int8_t>(g_features[i] ? one : zero);
    }

    if (g_interpreter->Invoke() != kTfLiteOk) {
        return kFailure;
    }

    const TfLiteTensor *output = g_interpreter->output(0);
    int top = 0;
    int second = 1;
    float top_probability = dequantized_value(output->data.int8[top], output);
    float second_probability = dequantized_value(output->data.int8[second], output);
    if (second_probability > top_probability) {
        int swap_index = top;
        float swap_probability = top_probability;
        top = second;
        top_probability = second_probability;
        second = swap_index;
        second_probability = swap_probability;
    }
    for (int i = 2; i < MOONCAT_INTENT_MODEL_LABEL_COUNT; i++) {
        float probability = dequantized_value(output->data.int8[i], output);
        if (probability > top_probability) {
            second = top;
            second_probability = top_probability;
            top = i;
            top_probability = probability;
        } else if (probability > second_probability) {
            second = i;
            second_probability = probability;
        }
    }

    prediction->confidence = top_probability;
    prediction->margin = top_probability - second_probability;
    prediction->intent = static_cast<mooncat_router_intent_e>(top);
    if (prediction->intent == MOONCAT_INTENT_FALLBACK ||
        prediction->confidence < MOONCAT_INTENT_MODEL_CONFIDENCE_THRESHOLD ||
        prediction->margin < MOONCAT_INTENT_MODEL_MARGIN_THRESHOLD) {
        prediction->intent = MOONCAT_INTENT_FALLBACK;
    }
    return kSuccess;
}
