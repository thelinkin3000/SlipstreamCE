"""The retained body effect keeps its authored brightness and rise."""
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from harness import constant, function, structure

ROOT = Path(__file__).resolve().parents[1]


def test_overshield_body_rendering(tmp_path):
    text = (ROOT / "port/linux/game/overshield_effect.c").read_text()
    code = r'''
#include <string.h>
typedef float real;
typedef int boolean;
typedef struct {real red, green, blue;} real_rgb_color;
#define FALSE 0
#define TRUE 1
#define NONE (-1)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define PIN(x,lo,hi) MIN(MAX(x,lo),hi)
#define TEST_FLAG(flags,bit) ((flags) & (1 << (bit)))
#define _object_dead_bit 0
#define _object_shield_over_charging_bit 1
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define CHECK(c) do {if (!(c)) return __LINE__;} while (0)
static boolean enabled = TRUE;
static boolean overshield_effect_enabled(void) { return enabled; }
static int near(real a, real b) {real d = a - b; return d > -.00001f && d < .00001f;}
'''
    code += f"\n#define OVERSHIELD_EFFECT_MAXIMUM {constant(text, 'OVERSHIELD_EFFECT_MAXIMUM')}\n"
    code += "\n".join(re.findall(r"^#define OVERSHIELD_EFFECT_(?:ENERGY_SPEED|EDGE|RISE|RISE_FROM_DOWN|DOWN) .*", text, re.M))
    code += "\n" + structure(text, "overshield_effect")
    code += "\nstatic struct overshield_effect overshield_effects[OVERSHIELD_EFFECT_MAXIMUM];\n"
    code += "\n".join(re.findall(r"static real_rgb_color const overshield_effect_(?:outer_)?rgb = .*?;", text))
    code += r"""
static boolean overshield_effects_initialized;
#define OVERSHIELD_EFFECT_MODELS 8
static struct {long model_index;} overshield_effect_models[OVERSHIELD_EFFECT_MODELS];
static short overshield_effect_next_model;
struct unit_datum {struct {real shield_vitality; unsigned long damage_flags;} object;};
static struct unit_datum unit;
static struct unit_datum *unit_try_and_get(long index) {return index == 7 ? &unit : NULL;}
static struct {real game_time_sec;} global_frame_parameters;
"""
    code += "\n" + function(text, "overshield_effect_reset")
    code += "\n" + function(text, "overshield_effect_layer_color")
    code += "\n" + function(text, "overshield_effect_get")
    code += "\n" + function(text, "overshield_effect_light_sample")
    code += "\n" + function(text, "overshield_effect_find_values")
    code += "\n" + function(text, "overshield_effect_draw")
    code += "\n" + function(text, "overshield_effect_color")
    renderer = (ROOT / "source/rasterizer/xbox/rasterizer_xbox_plasma_energy.c").read_text()
    draw = function(renderer, "rasterizer_plasma_energy_draw")
    colors = draw[draw.index("perpendicular_color = plasma->perpendicular_color;"):draw.index("rasterizer_set_texture(0,")]
    code += "\n" + function((ROOT / "port/linux/game/shield_color.c").read_text(), "shield_color_apply")
    code += r'''
static int selected_color;
static boolean shield_color_override(real_rgb_color const *colors, real_rgb_color *chosen) {
    if (!selected_color) return FALSE;
    chosen->red = chosen->green = 0.f;
    chosen->blue = selected_color == 1 ? 1.f : 0.f;
    return TRUE;
}
static void render_colors(real const *values, real_rgb_color *perpendicular, real_rgb_color *parallel) {
    struct {real_rgb_color perpendicular_color, parallel_color;} authored = {*perpendicular, *parallel};
    struct {real_rgb_color const *colors; real const *exponents;} parameters = {NULL, values};
    real_rgb_color perpendicular_color, parallel_color, chosen;
    real_rgb_color const *tint, *global_real_rgb_white = NULL;
'''
    code += colors.replace("plasma->", "authored.").replace("runtime->", "parameters.").replace("runtime && ", "").replace("runtime ? parameters.colors : NULL", "parameters.colors")
    code += "*perpendicular = perpendicular_color; *parallel = parallel_color;\n}\n"
    code += r'''
int main(void) {
    struct overshield_effect *effect = &overshield_effects[0];
    real own[4] = {0}, speed, softness, height, coverage;
    real_rgb_color light;
    real_rgb_color perpendicular = {.1f, .8f, .2f}, parallel = {.3f, .2f, .1f};
    real_rgb_color original_perpendicular = perpendicular, original_parallel = parallel;
    real layers[] = {3.f, 2.0001f, 2.f, 1.0001f, 2.5f};
    int layer, arms;
    effect->unit_index = 7;
    effect->shield_vitality = 2.f;
    effect->bottom = -1.f; effect->top = 1.f; effect->progress = .5f;
    CHECK(!overshield_effect_color(NULL, &perpendicular, &parallel));
    CHECK(!overshield_effect_color(own, &perpendicular, &parallel));
    CHECK(!memcmp(&perpendicular, &original_perpendicular, sizeof(perpendicular)));
    CHECK(!memcmp(&parallel, &original_parallel, sizeof(parallel)));
    CHECK(overshield_effect_draw(effect->body_values, &speed, &softness, &height));
    CHECK(near(speed, .3f) && near(softness, 1.f/.24f) && near(height, .12f));
    CHECK(overshield_effect_color(effect->body_values, &perpendicular, &parallel));
    CHECK(near(perpendicular.red, .8f) && near(perpendicular.green, .306f/.992f*.8f));
    CHECK(near(perpendicular.blue, .455f/.992f*.8f));
    CHECK(near(parallel.red, .3f) && near(parallel.green, .306f/.992f*.3f));
    CHECK(near(parallel.blue, .455f/.992f*.3f));
    effect->arms_values = own;
    for (layer = 0; layer < 5; layer++) {
        effect->shield_vitality = layers[layer];
        for (arms = 0; arms < 2; arms++) {
            perpendicular = original_perpendicular; parallel = original_parallel;
            CHECK(overshield_effect_color(arms ? own : effect->body_values, &perpendicular, &parallel));
            CHECK(near(layers[layer] > 2.f ? perpendicular.green : perpendicular.red, .8f));
            CHECK(near(layers[layer] > 2.f ? parallel.green : parallel.red, .3f));
        }
    }
    enabled = FALSE;
    perpendicular = original_perpendicular; parallel = original_parallel;
    CHECK(!overshield_effect_color(own, &perpendicular, &parallel));
    CHECK(!overshield_effect_color(effect->body_values, &perpendicular, &parallel));
    CHECK(!memcmp(&perpendicular, &original_perpendicular, sizeof(perpendicular)));
    enabled = TRUE;
    effect->arms_values = NULL;
    CHECK(!overshield_effect_color(own, &perpendicular, &parallel));
    effect->progress = 1.f;
    CHECK(overshield_effect_draw(effect->body_values, &speed, &softness, &height));
    CHECK(softness == 0 && height == 0);
    effect->unit_index = NONE;
    CHECK(!overshield_effect_draw(effect->body_values, &speed, &softness, &height));
    effect = &overshield_effects[OVERSHIELD_EFFECT_MAXIMUM-1];
    effect->unit_index = 8; effect->shield_vitality = 3.f; effect->arms_values = own;
    perpendicular = original_perpendicular; parallel = original_parallel;
    CHECK(overshield_effect_color(own, &perpendicular, &parallel));
    CHECK(near(perpendicular.green, .8f) && near(parallel.green, .3f));
    CHECK(overshield_effect_draw(own, &speed, &softness, &height));
    CHECK(near(speed, .3f) && softness == 0 && height == 0);
    effect->unit_index = NONE;
    CHECK(!overshield_effect_color(own, &perpendicular, &parallel));
    global_frame_parameters.game_time_sec = 1.f;
    unit.object.shield_vitality = 3.f;
    effect = overshield_effect_get(7); CHECK(effect && effect->shield_vitality == 3.f);
    CHECK(overshield_effect_light_sample(7, &light, &coverage) && light.green > light.red);
    unit.object.shield_vitality = 2.f;
    CHECK(overshield_effect_light_sample(7, &light, &coverage) && light.red > light.green);
    CHECK(overshield_effect_get(7) == effect && effect->shield_vitality == 2.f);
    unit.object.shield_vitality = 1.f;
    CHECK(!overshield_effect_get(7));
    CHECK(!overshield_effect_color(effect->body_values, &perpendicular, &parallel));
    unit.object.damage_flags = 1 << _object_shield_over_charging_bit;
    CHECK(overshield_effect_get(7)); /* retain the existing charging rise */
    unit.object.shield_vitality = 2.1f;
    CHECK(overshield_effect_get(7)->shield_vitality == 2.1f);
    enabled = FALSE; CHECK(!overshield_effect_get(7));
    CHECK(!overshield_effect_color(effect->body_values, &perpendicular, &parallel));
    enabled = TRUE; unit.object.damage_flags = 1 << _object_dead_bit;
    CHECK(!overshield_effect_get(7));
    overshield_effect_models[0].model_index = 17;
    overshield_effect_next_model = 5;
    overshield_effect_reset();
    CHECK(overshield_effect_models[0].model_index == NONE && overshield_effect_next_model == 0);
    CHECK(overshield_effects[0].unit_index == NONE && overshield_effects[0].arms_values == NULL);
    /* Even a sub-second rollback must discard the old rise and draw owner. */
    unit.object.damage_flags = 1 << _object_shield_over_charging_bit;
    unit.object.shield_vitality = 1.f;
    global_frame_parameters.game_time_sec = 10.f;
    effect = overshield_effect_get(7); CHECK(effect && effect->progress == 0.f);
    effect->arms_values = own;
    global_frame_parameters.game_time_sec = 10.5f;
    CHECK(overshield_effect_get(7)->progress > 0.f);
    global_frame_parameters.game_time_sec = 10.4f;
    effect = overshield_effect_get(7);
    CHECK(effect && effect->progress == 0.f && effect->arms_values == NULL);
    CHECK(!overshield_effect_get(8));
    effect = &overshield_effects[0]; effect->unit_index = 7; effect->shield_vitality = 3.f;
    selected_color = 1;
    perpendicular = original_perpendicular; parallel = original_parallel;
    render_colors(effect->body_values, &perpendicular, &parallel);
    CHECK(near(perpendicular.green, .8f) && near(parallel.green, .3f));
    /* Ordinary shields still use Shield Color, including its black fallback. */
    perpendicular = original_perpendicular; parallel = original_parallel;
    render_colors(own, &perpendicular, &parallel);
    CHECK(near(perpendicular.blue, .8f) && perpendicular.red == 0.f);
    selected_color = 2;
    perpendicular = original_perpendicular; parallel = original_parallel;
    render_colors(own, &perpendicular, &parallel);
    CHECK(near(perpendicular.red, .8f * .12f) && near(parallel.blue, .3f * .12f));
    selected_color = 0;
    perpendicular = original_perpendicular; parallel = original_parallel;
    render_colors(own, &perpendicular, &parallel);
    CHECK(!memcmp(&perpendicular, &original_perpendicular, sizeof(perpendicular)));
    enabled = FALSE; selected_color = 1;
    render_colors(effect->body_values, &perpendicular, &parallel);
    CHECK(near(perpendicular.blue, .8f) && perpendicular.green == 0.f);
    return 0;
}
'''
    compile_case(tmp_path, code)


def compile_case(tmp_path, code):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        pytest.skip("a C compiler is required")
    source = tmp_path / "shield.c"
    source.write_text(code)
    executable = tmp_path / ("shield.exe" if sys.platform == "win32" else "shield")
    flags = ["--target=i686-pc-windows-msvc", "-fuse-ld=lld"] if sys.platform == "win32" else ["-lm"]
    subprocess.run([compiler, "-std=gnu89", str(source), *flags, "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


def test_overshield_activation_and_lifetime(tmp_path):
    text = (ROOT / "port/linux/game/overshield_effect.c").read_text()
    code = r'''
#include <string.h>
#include <assert.h>
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define OVERSHIELD_EFFECT_MAXIMUM 16
#define OVERSHIELD_EFFECT_MODELS 8
#define _shield_effects_overshield 3
static struct {long unit_index; void *arms_values;} overshield_effects[16];
static struct {long model_index;} overshield_effect_models[8];
static short overshield_effect_next_model;
static boolean overshield_effects_initialized;
static unsigned long changes;
static int local, policy = NONE;
static unsigned long config_changes(void) {return changes;}
static int config_boolean(char const *key) {return local;}
static short network_shield_effects(void) {return policy;}
'''
    code += function(text, "overshield_effect_reset") + "\n" + function(text, "overshield_effect_enabled")
    code += r'''
int main(void) {
    int choice;
    assert(!overshield_effect_enabled());
    local = 1; changes++; assert(overshield_effect_enabled());
    overshield_effect_reset();
    overshield_effects[0].unit_index = 7;
    overshield_effects[0].arms_values = &local;
    overshield_effect_models[0].model_index = 3;
    overshield_effect_next_model = 5;
    policy = 0; assert(!overshield_effect_enabled());
    assert(!overshield_effects_initialized && overshield_effects[0].unit_index == NONE);
    assert(!overshield_effects[0].arms_values && overshield_effect_models[0].model_index == NONE);
    assert(overshield_effect_next_model == 0);
    for (choice = 0; choice < 6; choice++) {
        policy = choice; assert(overshield_effect_enabled() == (choice >= 3));
    }
    local = 0; changes++; policy = 3; assert(overshield_effect_enabled());
    policy = NONE; assert(!overshield_effect_enabled());
    local = 1; changes++; assert(overshield_effect_enabled());
    return 0;
}
'''
    compile_case(tmp_path, code)
    # (the map's light lifetimes reset it through Shield Glow's reset, which owns the overshield's glow)
    lights = (ROOT / "source/objects/object_lights.c").read_text()
    glow = (ROOT / "port/linux/game/shield_glow.c").read_text()
    assert "overshield_effect_reset();" in function(glow, "shield_glow_reset")
    for name in ("lights_initialize_for_new_map", "lights_dispose_from_old_map", "lights_dispose"):
        assert "shield_glow_reset();" in function(lights, name)
