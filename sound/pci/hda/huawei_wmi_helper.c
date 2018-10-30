// SPDX-License-Identifier: GPL-2.0
/* Helper functions for Huawei Mic Mute LED control;
 * to be included from codec driver
 */
 
#if IS_ENABLED(CONFIG_HUAWEI_LAPTOP)
#include <linux/huawei_wmi.h>

static int (*huawei_wmi_micmute_led_set_func)(bool);

static void update_huawei_wmi_micmute_led(struct hda_codec *codec,
				      struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	if (!ucontrol || !huawei_wmi_micmute_led_set_func)
		return;
	if (strcmp("Capture Switch", ucontrol->id.name) == 0 && ucontrol->id.index == 0) {
		/* TODO: How do I verify if it's a mono or stereo here? */
		bool val = ucontrol->value.integer.value[0] || ucontrol->value.integer.value[1];
		huawei_wmi_micmute_led_set_func(!val);
	}
}
 
static void alc_fixup_huawei_wmi(struct hda_codec *codec,
			       const struct hda_fixup *fix, int action)
{
	struct hda_gen_spec *spec = codec->spec;
	bool removefunc = false;
	
	codec_info(codec, "In alc_fixup_huawei_wmi\n");

	if (action == HDA_FIXUP_ACT_PROBE) {
		if (!huawei_wmi_micmute_led_set_func)
			huawei_wmi_micmute_led_set_func = symbol_request(huawei_wmi_micmute_led_set);
		if (!huawei_wmi_micmute_led_set_func) {
			codec_warn(codec, "Failed to find huawei_wmi symbol huawei_wmi_micmute_led_set\n");
			return;
		}

		removefunc = true;
		if (huawei_wmi_micmute_led_set_func(false) >= 0) {
			if (spec->num_adc_nids > 1 && !spec->dyn_adc_switch)
				codec_dbg(codec, "Skipping micmute LED control due to several ADCs");
			else {
				spec->cap_sync_hook = update_huawei_wmi_micmute_led;
				removefunc = false;
			}
		}
	    codec_info(codec, "In alc_fixup_huawei_wmi IF\n");

	}

	if (huawei_wmi_micmute_led_set_func && (action == HDA_FIXUP_ACT_FREE || removefunc)) {
		symbol_put(huawei_wmi_micmute_led_set);
		huawei_wmi_micmute_led_set_func = NULL;
	}
}
 
#else

static void hda_fixup_huawei_wmi(struct hda_codec *codec,
			       const struct hda_fixup *fix, int action)
{
}
 
#endif
