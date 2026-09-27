#pragma once
#include "settings.h"
namespace mmvr {
enum class Form { FierceDeity = 0, Goron = 1, Zora = 2, Deku = 3, Human = 4 };
struct FormProfile {
    Form id;
    const char* name;
    Setting eyeHeight;
    bool humanItems;
};
inline constexpr FormProfile FormProfiles[] = { { Form::FierceDeity, "Fierce Deity", Setting::DeityEyeHeight, false },
                                                { Form::Goron, "Goron", Setting::GoronEyeHeight, false },
                                                { Form::Zora, "Zora", Setting::ZoraEyeHeight, false },
                                                { Form::Deku, "Deku", Setting::DekuEyeHeight, false },
                                                { Form::Human, "Human", Setting::EyeHeight, true } };
inline const FormProfile* ProfileForForm(int form) {
    return form >= 0 && form < 5 ? &FormProfiles[form] : nullptr;
}
inline bool IsEyeHeightSetting(int id) {
    for (const auto& profile : FormProfiles) if (int(profile.eyeHeight) == id) return true;
    return false;
}
inline float AdjustedEyeHeight(const Settings& settings, Setting id, float modelHeight) {
    const float nominal = SettingDefinitions[size_t(id)].initial;
    const float base = settings.Get(Setting::ModelFormHeight) > .5f && modelHeight > 0 ? modelHeight : nominal;
    return std::max(4.f, base + settings.Get(id) - nominal);
}
// Floor-pinned world scale: form eye height over the physical eye height in
// game units (40 per metre times the configured Player Height). Clamped to a
// comfortable stereo range.
inline bool FloorPinnedWorldScaleActive(const Settings& settings) {
    return settings.Get(Setting::FloorPinnedWorldScale) > .5f;
}
inline float WorldScaleForEyes(float formEye, float physicalEyeUnits) {
    if (!std::isfinite(formEye) || !std::isfinite(physicalEyeUnits) || physicalEyeUnits < 4.f)
        return 1.f;
    return std::clamp(formEye / physicalEyeUnits, 0.25f, 3.f);
}
} // namespace mmvr
