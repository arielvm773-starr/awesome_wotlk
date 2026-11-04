#include "Misc.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "D3D.h"
#include "Extensions.h"
#include "MiscCaptureFrame.h"
#include "MiscCaptureFrameLua.h"
#include "MiscGroundTargetClamp.h"
#include "MiscInteraction.h"
#include "MiscProjectedLayers.h"
#include "Utils.h"

#include "include/DB/DBRecords.h"
#include "include/FrameScript/FrameScript.h"
#include "include/Game/CGGameUI.h"
#include "include/Graphics/CGxDevice.h"
#include "include/Graphics/CGxDeviceD3d.h"
#include "include/Graphics/ProjectedTexture.h"
#include "include/Lib/Lua.h"
#include "include/M2/CM2Model.h"
#include "include/Math/Primitives.h"
#include "include/ObjectManager/CGGameObject_C.h"
#include "include/ObjectManager/CGPlayer_C.h"
#include "include/ObjectManager/ObjectManager.h"
#include "include/ObjectManager/ObjectManagerEnums.h"
#include "include/Spell/CSpell_C.h"
#include "include/System/System.h"
#include "include/Texture/CTexture.h"
#include "include/UI/UIBindings.h"
#include "include/World/CGWorldFrame.h"

namespace {
int luaQueueInteract(LuaState* l) {
    if (!game_ui::isInWorld()) { return 0; }
    if (!lua::isNoneOrNil(l, 1)) {
        const char* raw = lua::toString(l, 1);
        if (raw == nullptr) { return 0; }
        std::string mod_str = raw;

        for (char c : mod_str) {
            if (std::isalnum(static_cast<unsigned char>(c)) == 0) { return 0; }
        }
        misc_interaction::interact.queue(object_mgr::guidFromUnitId{}(mod_str.c_str()));
    } else {
        misc_interaction::interact.queueCandidate();
    }
    return 0;
}

int interactFunctionC(LuaState* l) {
    const char* param = nullptr;
    if (!lua::isNoneOrNil(l, 1)) { param = lua::toString(l, 1); }

    lua::pushCFunction(l, reinterpret_cast<lua::LuaCFunction>(framescript::secureCmdOptionsParse::kAddress));

    if (param != nullptr) {
        lua::pushString(l, param);
    } else {
        lua::pushNil(l);
    }

    if (lua::pcall(l, 1, 2, 0) != 0) {
        lua::pop(l, 1);
        lua::pushCFunction(l, luaQueueInteract);
        if (lua::isFunction(l, -1)) {
            if (lua::pcall(l, 0, 0, 0) != 0) { lua::pop(l, 1); }
        }
        return 0;
    }

    if (!lua::isNil(l, -1)) {
        lua::pushCFunction(l, luaQueueInteract);
        if (lua::isFunction(l, -1)) {
            lua::pushValue(l, -2);
            if (lua::pcall(l, 1, 0, 0) != 0) { lua::pop(l, 1); }
        }
        lua::pop(l, 3);
    } else {
        lua::pop(l, 1);
        lua::pushCFunction(l, luaQueueInteract);
        if (lua::isFunction(l, -1)) {
            if (lua::pcall(l, 0, 0, 0) != 0) { lua::pop(l, 1); }
        }
        lua::pop(l, 1);
    }
    return 0;
}
}  // namespace

namespace {
enum ObjHlMode : int {
    eHlDisabled,
    eHlAlways,
    eHlTracked,
};

enum PendingClick : int { eNone, eCursor, ePlayerLocation, eBlocked };

auto pending_click = PendingClick::eNone;

// where the last hover left the circle, in world space. Only a hover writes it: a click has to land on that circle
std::optional<Vec3f> shown_circle;

HOOKKIT_BIND(framescript::secureCmdOptionsParse_hook, [](LuaState* l) {
    int result = framescript::secureCmdOptionsParse{}(l);
    if (lua::getTop(l) < 3 || !lua::isString(l, 2) || !lua::isString(l, 3)) { return result; }

    if (!CSpell_C::isTargetingAoE{}()) { pending_click = PendingClick::eNone; }
    if (pending_click == PendingClick::eBlocked) { return result; }

    std::string_view parsed_target_view = lua::toString(l, 3);
    bool is_cursor = utils::iequals(parsed_target_view, "cursor");
    bool is_playerlocation = utils::iequals(parsed_target_view, "playerlocation");

    if (!is_cursor && !is_playerlocation) { return result; }

    pending_click = is_cursor ? PendingClick::eCursor : PendingClick::ePlayerLocation;

    std::string parsed_result = lua::toString(l, 2);
    std::string orig_string = lua::toString(l, 1);

    lua::pop(l, 3);
    lua::pushString(l, orig_string.c_str());
    lua::pushString(l, parsed_result.c_str());
    lua::pushNil(l);

    return result;
});

HOOKKIT_BIND(CGWorldFrame::onLayerTrackTerrain_hook, [](CGWorldFrame* self, CGWorldFrame::TerrainClickEvent* click) {
    if (CSpell_C::isTargetingAoE{}()) {
        if (pending_click == PendingClick::eNone) {
            pending_click = PendingClick::eBlocked;
            return self->onLayerTrackTerrain(click);
        }
        if (pending_click == PendingClick::eBlocked) { return self->onLayerTrackTerrain(click); }
    }

    auto* player = object_mgr::get<CGPlayer_C>(object_mgr::getPlayerGuid(), eTypemaskPlayer);
    if (player == nullptr) { return self->onLayerTrackTerrain(click); }

    PendingClick pending = std::exchange(pending_click, PendingClick::eNone);
    if (pending == PendingClick::ePlayerLocation) {
        Vec3f player_pos{};
        player->getPosition(player_pos);
        CGWorldFrame::leftClickTerrain(0, player_pos);
    } else if (pending == PendingClick::eCursor) {
        CGWorldFrame::leftClickTerrain(click->guid, click->pos);
    }
    return self->onLayerTrackTerrain(click);
});

HOOKKIT_BIND(CSpell_C::cancelPendingAoeTargeting_hook, []() {
    pending_click = PendingClick::eNone;
    shown_circle.reset();
    return CSpell_C::cancelPendingAoeTargeting{}();
});
}  // namespace

namespace {
struct EngineWorld {
    // the cursor's own pick
    static bool pick(const Vec3f& from, const Vec3f& to, Vec3f& hit, uint64_t& object) {
        return CGWorldFrame::pickWorld(
            from, to, CGWorldFrame::kCursorTerrainFlags, CGWorldFrame::kCursorCollisionFlags, hit, object);
    }

    // only ordinary doodads go
    static bool surface(const Vec3f& from, const Vec3f& to, Vec3f& hit, uint64_t& object) {
        constexpr uint32_t kDrawn = CGWorldFrame::kCursorTerrainFlags & ~CGWorldFrame::kDoodadHitFlags;
        constexpr uint32_t kCollision = CGWorldFrame::kCursorCollisionFlags & ~CGWorldFrame::kDoodadHitFlags;
        return CGWorldFrame::pickWorld(from, to, kDrawn, kCollision, hit, object);
    }

    static bool visible(Vec3f from, Vec3f to, Vec3f& hit) {
        float dist = 0.0f;
        return math::trace(from, to, CGWorldFrame::kCursorTraceFlags, hit, dist);
    }
};

// none when no terrain spell follows this hit
std::optional<misc_ground_target_clamp::Query> clampQuery(
    CGWorldFrame* self, const CGWorldFrame::HitTestResult& hit, int kind) {
    if (kind == CGWorldFrame::eHitNothing || kind == CGWorldFrame::eHitModel || !CSpell_C::isTargetingAoE{}() ||
        !CSpell_C::canTargetTerrain{}()) {
        return std::nullopt;
    }

    const auto* cast = CSpell_C::pending_spell_cast;
    auto* unit = object_mgr::get<CGUnit_C>(cast->unit_guid, eTypemaskUnit);
    if (unit == nullptr) { return std::nullopt; }

    misc_ground_target_clamp::Query query{.ray_start = hit.ray_start, .shown = shown_circle};
    unit->getPosition(query.caster);
    self->camera_->getForwardVector(&query.camera_forward);
    CSpell_C::getSpellRange{}(unit, cast->spell_id, &query.min_range, &query.max_range, nullptr);
    return query;
}

// true when the hit moved, it then is terrain whatever it was before
bool clampTerrainHit(const misc_ground_target_clamp::Query& query, CGWorldFrame::HitTestResult& hit) {
    misc_ground_target_clamp::GroundHit ground;
    if (!misc_ground_target_clamp::clamp<EngineWorld>(query, hit.worldPos(), ground)) { return false; }

    hit.setWorldPos(ground.pos, ground.object);
    return true;
}

// only the world frame's hover
HOOKKIT_CALLSITE_HOOK(hoverHitTest_call, 0x004FA13A, CGWorldFrame::hitTestPoint_hook);

CGWorldFrame::HitTestKind clampedHoverHitTest(
    CGWorldFrame* self, float x, float y, uint32_t filter, CGWorldFrame::HitTestResult* hit) {
    const CGWorldFrame::HitTestKind kind = self->hitTestPoint(x, y, filter, hit);
    const auto query = clampQuery(self, *hit, kind);
    shown_circle.reset();
    if (!query) { return kind; }

    const bool moved = clampTerrainHit(*query, *hit);
    const Vec3f pos = hit->worldPos();
    if (moved || (pos - query->caster).lengthSq() <= query->max_range * query->max_range) { shown_circle = pos; }
    return moved ? CGWorldFrame::eHitTerrain : kind;
}

int clampedDefaultAction(CGWorldFrame* self, CGWorldFrame::ClickButton button) {
    if (button == CGWorldFrame::eClickButtonLeft) {
        const auto query = clampQuery(self, self->default_action_hit_, self->pending_action_id_);
        if (query && clampTerrainHit(*query, self->default_action_hit_)) {
            self->pending_action_id_ = CGWorldFrame::eHitTerrain;
        }
    }
    return self->performDefaultAction(button);
}

void applyGroundTargetClamp(int enabled) {
    const bool on = enabled != 0;
    static_cast<void>(hookkit::HookTransaction::reinstallIf<clampedHoverHitTest>(hoverHitTest_call{}, on));
    static_cast<void>(
        hookkit::HookTransaction::reinstallIf<clampedDefaultAction>(CGWorldFrame::performDefaultAction_hook{}, on));
}
}  // namespace

namespace {
class ReachStage final : public CGxTexExt::Source {
public:
    void bind(CGxDevice* device, const misc_projected_layers::ProjectedLayers::ReachMask& mask, const Vec3f& camera) {
        if (device->caps_.num_tmus <= kStage || !refresh(mask)) { return; }

        const Mat4f matrix = misc_projected_layers::reachMatrix(mask, camera);
        device->xformPush(eGxXformTex2, &matrix);
        bound_ = true;

        const auto set = [device](uint32_t state, void* value) {
            device->rsSet(static_cast<GxRenderState>(state + kStage), value);
        };
        set(eGxRsColorOp0, reinterpret_cast<void*>(eGxTexOpMod));
        set(eGxRsAlphaOp0, reinterpret_cast<void*>(eGxTexOpMod));
        set(eGxRsTexGen0, reinterpret_cast<void*>(eGxTexGenWorldPosition));
        set(eGxRsTexMatrixMode0, reinterpret_cast<void*>(eGxTexMatrixModeAffine));
        set(eGxRsTexture0 + 1, nullptr);
        set(eGxRsTexture0, texture_);
    }

    void unbind(CGxDevice* device) {
        if (std::exchange(bound_, false)) { device->popXform(eGxXformTex2); }
    }

    const void* pixels(int32_t, int32_t, uint32_t& pitch) override {
        pitch = misc_projected_layers::kReachTexels * sizeof(uint32_t);
        return texels_.data();
    }

private:
    static constexpr uint32_t kStage = 2;

    [[nodiscard]]
    bool refresh(const misc_projected_layers::ProjectedLayers::ReachMask& mask) {
        using namespace misc_projected_layers;
        if (texture_ == nullptr) {
            texels_.fill(kReachEmpty);
            GxTexFlags flags{};
            flags.unpacked.filter_mode = eGxTexFilterLinear;
            texture_ = CGxTexExt::create({.width = kReachTexels, .height = kReachTexels, .flags = flags}, this);
            if (texture_ == nullptr) { return false; }
            texture_->markDirty();
        }
        if (mask.covered != shown_) {
            shown_ = mask.covered;
            writeReachTexels(mask, texels_);
            constexpr int kEnd = kReachMargin + ProjectedLayers::kColumns;
            texture_->markDirty({{.x0 = kReachMargin, .y0 = kReachMargin, .x1 = kEnd, .y1 = kEnd}});
        }
        return true;
    }

    CGxTexExt* texture_ = nullptr;
    bool bound_ = false;
    decltype(misc_projected_layers::ProjectedLayers::ReachMask::covered) shown_{};
    std::array<uint32_t, misc_projected_layers::kReachTexels * misc_projected_layers::kReachTexels> texels_{};
};

constexpr utils::Accessor<ReachStage, struct ReachStageTag> kReachStage;

HOOKKIT_BIND(CGxDevice::projectTex2d_hook,
    [](AaBox* bbox, Vec4u8* color, Mat4f* matrix, float z_bias, int flags, char vtx_mode, float depth_bias) {
        AaBox new_bbox = *bbox;

        // expand Z projection range to prevent clipping on steep terrain; a blob shadow's box isn't square
        const float center_z = (new_bbox.max.z + new_bbox.min.z) * 0.5f;
        const float half = misc_projected_layers::bandHalf(
            std::max(new_bbox.max.x - new_bbox.min.x, new_bbox.max.y - new_bbox.min.y) * 0.5f,
            (new_bbox.max.z - new_bbox.min.z) * 0.5f);
        new_bbox.min.z = center_z - half;
        new_bbox.max.z = center_z + half;

        // its liquid pass has no ground bit
        const bool reticle =
            projected_texture::isAoeProj(matrix, vtx_mode) && (flags & CGWorldFrame::eHitTestGround) != 0;
        misc_projected_layers::layers.arm(new_bbox, z_bias, reticle);
        void* const result = CGxDevice::projectTex2d{}(&new_bbox, color, matrix, z_bias, flags, vtx_mode, depth_bias);
        kReachStage->unbind(CGxDevice::getDevice());
        return result;
    });

// the ramp's matrix is pushed and nothing has drawn with it yet
HOOKKIT_CALLSITE_HOOK(projectedDraw_rsPush_call, 0x007E3EB4, CGxDevice::rsPush_hook);

HOOKKIT_BIND(projectedDraw_rsPush_call, [](CGxDevice* self) {
    using misc_projected_layers::layers;
    const std::optional<misc_projected_layers::RampPlane> plane = layers.solve(projected_texture::gatheredBatches());
    if (!plane) {
        self->rsPush();
        return;
    }

    const Vec3f& camera = CGWorldFrame::get()->camera_->pos_;
    self->setXform(eGxXformTex1, misc_projected_layers::rampMatrix(*plane, camera, layers.zBias()));
    self->rsPush();
    kReachStage->bind(self, layers.reachMask(), camera);
});

void applyProjectedMode(int enabled) {
    const bool on = enabled != 0;
    if (on == CGxDevice::projectTex2d_hook::attached.load()) { return; }

    hookkit::HookTransaction tx;
    if (!tx) { return; }
    if (on) {
        tx.attach(CGxDevice::projectTex2d_hook{}, projectedDraw_rsPush_call{});
    } else {
        tx.detach(CGxDevice::projectTex2d_hook{}, projectedDraw_rsPush_call{});
    }
    tx.forceWrite<uint32_t>(projected_texture::kM2DecalPadRefAddr,
        on ? projected_texture::kM2DecalPadWideAddr : projected_texture::kM2DecalPadAddr);
    static_cast<void>(tx.commit());
}
}  // namespace

namespace {
// attached only while interactionHighlight is on
HOOKKIT_BIND(CGObject_C::hideHighlightType_hook, [](CGObject_C* self, CGObject_C::HighlightType highlight_type) {
    int result = self->hideHighlightType(highlight_type);
    misc_interaction::interact.onHighlightHidden(self, highlight_type);
    return result;
});

void applyInteractionHighlight(int enabled) {
    static_cast<void>(hookkit::HookTransaction::reinstallIf(CGObject_C::hideHighlightType_hook{}, enabled != 0));
}

// one detour per objectHighlightMode other than eHlDisabled, which runs the engine's
template <ObjHlMode Mode>
void passiveHighlight(CGGameObject_C* self) {
    self->checkForPassiveHighlight();

    GameobjectTypes go_type = self->descriptors_->subtype;
    if (!self->canUse() ||
        (go_type != eGameobjectTypeChest && go_type != eGameobjectTypeGoober && go_type != eGameobjectTypeQuestgiver)) {
        return;
    }
    if constexpr (Mode == ObjHlMode::eHlTracked) {
        if (go_type == eGameobjectTypeQuestgiver && self->quest_model_ == nullptr) { return; }
        if (go_type == eGameobjectTypeChest) {
            if (const LockRec* lock_rec = self->getLockRec()) {
                if (lock_rec->type[0] == LockRec::eLockKeySkill) { return; }  // gathering node
            }
        }
    }
    self->highlight_mask_ |= CGObject_C::eHighlightMaskPassiveLootGlow;
    self->showLootEffect();
}
}  // namespace

namespace {
// selfHighlight modes: 1 = interact glow, 2 = native target outline, 3 = native object-track outline
CGObject_C* getLocalPlayerObject() {
    guid_t player_guid = object_mgr::getPlayerGuid();
    if (player_guid == 0) { return nullptr; }
    return object_mgr::get<CGObject_C>(player_guid, eTypemaskPlayer);
}

struct SelfHlStyle {
    CGObject_C::HighlightType type;
    uint32_t mask;
};

constexpr std::array<SelfHlStyle, 3> kSelfHlStyles = {{
    {CGObject_C::eHighlightTypeInteract, CGObject_C::eHighlightMaskInteract},
    {CGObject_C::eHighlightTypeLockedTarget, CGObject_C::eHighlightMaskLockedTarget},
    {CGObject_C::eHighlightTypeObjectTrack, CGObject_C::eHighlightMaskObjectTrack},
}};

void setSelfHighlight(int mode) {
    if (!game_ui::isInWorld()) { return; }
    CGObject_C* player = getLocalPlayerObject();
    if (player == nullptr) { return; }

    for (int i = 0; i < static_cast<int>(kSelfHlStyles.size()); ++i) {
        const auto& style = kSelfHlStyles[i];
        const bool want = (mode == i + 1);
        const bool active = (player->highlight_mask_ & style.mask) != 0;
        if (want && !active) {
            player->showHighlightType(style.type);
        } else if (!want && active) {
            player->hideHighlightType(style.type);
        }
    }
}

void applySelfHighlight(int mode) { setSelfHighlight(mode); }

// the model the engine tints for highlights (same virtual call showHighlightType uses)
CM2Model* getHighlightModel(CGObject_C* obj) {
    using GetModelFn = CM2Model*(__thiscall*)(CGObject_C*);
    void** vtable = *reinterpret_cast<void***>(obj);
    return reinterpret_cast<GetModelFn>(vtable[0xD4 / sizeof(void*)])(obj);
}

// the engine copies this RGB (floats at model+0x18C) into the additive light colour each frame
void applySelfHighlightColor(CGObject_C* player) {
    CM2Model* model = getHighlightModel(player);
    if (model == nullptr) { return; }
    auto& reg = *extensions::console::kCvarRegistry;
    const float power = static_cast<float>(reg.ref<"selfHighlightPower", int>()) / 100.0f;
    float* rgb = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(model) + 0x18C);
    rgb[0] = static_cast<float>(reg.ref<"selfHighlightR", int>()) / 255.0f * power;
    rgb[1] = static_cast<float>(reg.ref<"selfHighlightG", int>()) / 255.0f * power;
    rgb[2] = static_cast<float>(reg.ref<"selfHighlightB", int>()) / 255.0f * power;
}

// the engine drops highlights on model reload, mount, shapeshift etc., so re-assert it each frame
void updateSelfHighlight() {
    const int mode = extensions::console::kCvarRegistry->ref<"selfHighlight", int>();
    if (mode == 0 || !game_ui::isInWorld()) { return; }
    setSelfHighlight(mode);
    if (CGObject_C* player = getLocalPlayerObject()) { applySelfHighlightColor(player); }
}
}  // namespace

// Per-unit glow for addons: SetUnitGlow(unit[, r, g, b, power]), ClearUnitGlow(unit), ClearAllUnitGlows().
// r/g/b are 0..1. Works on any player the client has loaded, including yourself.
namespace {
struct UnitGlow {
    float r, g, b;
};
std::unordered_map<guid_t, UnitGlow> unit_glows;

constexpr auto kGlowType = CGObject_C::eHighlightTypeObjectTrack;
constexpr uint32_t kGlowMask = CGObject_C::eHighlightMaskObjectTrack;

CGObject_C* glowObject(guid_t guid) { return object_mgr::get<CGObject_C>(guid, eTypemaskPlayer); }

void releaseGlow(guid_t guid) {
    CGObject_C* obj = glowObject(guid);
    if (obj != nullptr && (obj->highlight_mask_ & kGlowMask) != 0) { obj->hideHighlightType(kGlowType); }
}

void updateUnitGlows() {
    if (unit_glows.empty() || !game_ui::isInWorld()) { return; }
    for (const auto& [guid, glow] : unit_glows) {
        CGObject_C* obj = glowObject(guid);
        if (obj == nullptr) { continue; }
        if ((obj->highlight_mask_ & kGlowMask) == 0) { obj->showHighlightType(kGlowType); }
        CM2Model* model = getHighlightModel(obj);
        if (model == nullptr) { continue; }
        float* rgb = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(model) + 0x18C);
        rgb[0] = glow.r;
        rgb[1] = glow.g;
        rgb[2] = glow.b;
    }
}

void clearUnitGlows() {
    for (const auto& entry : unit_glows) {
        releaseGlow(entry.first);
    }
    unit_glows.clear();
}

int luaSetUnitGlow(LuaState* l) {
    if (!lua::isString(l, 1)) { lua::throwError(l, "Usage: SetUnitGlow(unitID [, r, g, b, power])"); }
    const guid_t guid = object_mgr::string2Guid(lua::checkString(l, 1));
    if (guid == 0) { return 0; }
    auto num = [&](int idx, double def) { return lua::isNumber(l, idx) ? lua::toNumber(l, idx) : def; };
    const float power = static_cast<float>(std::clamp(num(5, 1.0), 0.0, 3.0));
    unit_glows[guid] = UnitGlow{
        static_cast<float>(std::clamp(num(2, 1.0), 0.0, 1.0)) * power,
        static_cast<float>(std::clamp(num(3, 0.55), 0.0, 1.0)) * power,
        static_cast<float>(std::clamp(num(4, 0.0), 0.0, 1.0)) * power,
    };
    return 0;
}

int luaClearUnitGlow(LuaState* l) {
    if (!lua::isString(l, 1)) { lua::throwError(l, "Usage: ClearUnitGlow(unitID)"); }
    const guid_t guid = object_mgr::string2Guid(lua::checkString(l, 1));
    if (guid == 0) { return 0; }
    if (unit_glows.erase(guid) != 0) { releaseGlow(guid); }
    return 0;
}

int luaClearAllUnitGlows(LuaState*) {
    clearUnitGlows();
    return 0;
}

int luaOpenGlow(LuaState* l) {
    static constexpr std::array<lua::LuaLReg, 3> kFuncs = {{
        {.name = "SetUnitGlow", .func = luaSetUnitGlow},
        {.name = "ClearUnitGlow", .func = luaClearUnitGlow},
        {.name = "ClearAllUnitGlows", .func = luaClearAllUnitGlows},
    }};
    for (const auto& [name, func] : kFuncs) {
        lua::pushCFunction(l, func);
        lua::setGlobal(l, name);
    }
    return 0;
}
}  // namespace

namespace {
constexpr int kEnginePortraitRes = 64;
int portrait_res = kEnginePortraitRes;

void applyPortraitRes(int res) {
    portrait_res = static_cast<int>(std::bit_ceil(static_cast<unsigned int>(res)));

    CTexture*& depth = game_ui::portrait_depth_texture;
    if (depth == nullptr || std::cmp_less_equal(portrait_res, depth->width_)) { return; }
    const auto size = static_cast<uint32_t>(portrait_res);
    // the engine's flags as AllocAndRegister stored them; it re-derives filter and anisotropy the same way again
    const uint32_t flags = depth->creation_flags_.packed;
    auto* callback = reinterpret_cast<void*>(game_ui::kPortraitDepthCallbackAddr);
    if (CTexture* grown =
            CTexture::allocAndRegister{}(0, size, size, 0x18, 12, 12, flags, nullptr, callback, "PortraitDepth", 1)) {
        CHandle::close{}(reinterpret_cast<CHandle*>(std::exchange(depth, grown)));
    }
}

HOOKKIT_NAMED_BIND_RAW(portraitInitialize_site, 0x006180E0, 0x006180E5) {
    constexpr uintptr_t kJmpback = portraitInitialize_site::target();
    const auto res = reinterpret_cast<uintptr_t>(&portrait_res);
    return CallsiteTrampolineBuilder{}.assertOnBuildFailure().build(
        jmpTo(kJmpback, movRegAbs(Reg::eDx, res), pushReg(Reg::eDx), pushReg(Reg::eDx), pushReg(Reg::eSi)));
};

HOOKKIT_NAMED_BIND_RAW(portraitRender_site, 0x00619B6A, 0x00619B72) {
    constexpr uintptr_t kJmpback = portraitRender_site::target();
    const uintptr_t size = HOOKKIT_LAMBDA_ADDR([] {
        if (game_ui::portrait_use_render_target == 0) { return kEnginePortraitRes; }
        const CTexture* depth = game_ui::portrait_depth_texture;
        return depth != nullptr ? std::min<int>(portrait_res, depth->width_) : portrait_res;
    });
    return CallsiteTrampolineBuilder{}.assertOnBuildFailure().build(
        size, jmpTo(kJmpback, pushImm(2), pushImm(2), pushReg(Reg::eAx), pushReg(Reg::eAx)), 0);
};
}  // namespace

namespace {
void applyChatLogStamp(int enabled) {
    static std::array<char, MAX_PATH> chat_log_path{};

    const char* path;
    if (enabled != 0) {
        auto [ptr, count] = std::format_to_n(
            chat_log_path.data(), chat_log_path.size() - 1, "Logs\\{}WoWChatLog.txt", utils::sessionStamp());
        *ptr = '\0';
        path = chat_log_path.data();
    } else {
        path = game_ui::kDefaultChatLogFile;
    }

    game_ui::log_file_names[game_ui::eLogChat] = path;
}

void applyCombatLogStamp(int enabled) {
    static std::array<char, MAX_PATH> combat_log_path{};

    const char* path;
    if (enabled != 0) {
        auto [ptr, count] = std::format_to_n(
            combat_log_path.data(), combat_log_path.size() - 1, "Logs\\{}WoWCombatLog.txt", utils::sessionStamp());
        *ptr = '\0';
        path = combat_log_path.data();
    } else {
        path = game_ui::kDefaultCombatLogFile;
    }

    game_ui::log_file_names[game_ui::eLogCombat] = path;
}

void applyTabTargetDistance(float distance, bool changed) {
    if (!changed) { return; }
    hookkit::forceWrite<float>(game_ui::kTabTargetRangeCapSqAddr, distance * distance);
}

void applyObjHlMode(int mode, bool changed) {
    auto new_mode = static_cast<ObjHlMode>(mode);

    using Hook = CGGameObject_C::checkForPassiveHighlight_hook;
    void* detour = nullptr;
    if (new_mode == eHlAlways) {
        detour = Hook::staticDetour<passiveHighlight<eHlAlways>>();
    } else if (new_mode == eHlTracked) {
        detour = Hook::staticDetour<passiveHighlight<eHlTracked>>();
    }
    const bool switched = Hook::attached.load() ? Hook::resolveDetour() != detour : detour != nullptr;
    if (switched && hookkit::HookTransaction::reinstall(Hook{}, detour) != NO_ERROR) { return; }
    if (!changed && !switched) { return; }

    if (guid_t player_guid = object_mgr::getPlayerGuid();
        object_mgr::get<CGPlayer_C>(player_guid, eTypemaskPlayer) != nullptr) {
        object_mgr::enumObjects([&](guid_t guid) {
            if (guid < 0x1000) { return true; }

            auto* go = object_mgr::get<CGGameObject_C>(guid, eTypemaskGameobject);
            if (go == nullptr || go->type_id_ != eTypeidGameobject) { return true; }

            if (new_mode != eHlDisabled) {
                CGGameObject_C::checkForPassiveHighlight_hook::viaDetour(go);
            } else {
                go->checkForPassiveHighlight();
            }
            return true;
        });
    }
}
}  // namespace

namespace {
int luaFlashWindow(LuaState*) {
    if (HWND hwnd = os::getGameWindow()) { FlashWindow(hwnd, FALSE); }
    return 0;
}

int luaIsWindowFocused(LuaState* l) {
    HWND hwnd = os::getGameWindow();
    if (hwnd == nullptr || GetForegroundWindow() != hwnd) { return 0; }
    lua::pushNumber(l, 1.0);
    return 1;
}

int luaFocusWindow(LuaState*) {
    if (HWND hwnd = os::getGameWindow()) { SetForegroundWindow(hwnd); }
    return 0;
}

int luaCopyToClipboard(LuaState* l) {
    const char* str = lua::checkString(l, 1);
    if (str != nullptr) {
        const std::string_view text{str};
        if (!text.empty()) { utils::copyToClipboardU8(str, nullptr); }
    }
    return 0;
}

int luaCaptureFrame(LuaState* l) {
    CSimpleFrame* frame = lua::toFrame(l);
    if (frame == nullptr) { return 0; }
    if (frame == CGWorldFrame::get()) {
        misc_capture_frame::frame_capture.captureFailed("FRAMECAPTURE_WORLD_FRAME");
        return 0;
    }
    misc_capture_frame::CaptureRequest request{.frame = frame};
    if (lua::isNumber(l, 2)) {
        request.longest_side = static_cast<float>(lua::toNumber(l, 2));
    } else if (lua::isString(l, 2)) {
        const std::string_view text = lua::toString(l, 2);
        const bool is_scale = !text.empty() && (text.back() == 'x' || text.back() == 'X');
        const std::string_view number = is_scale ? text.substr(0, text.size() - 1) : text;
        const char* const last = std::to_address(number.end());
        double value = 0.0;
        const auto [ptr, ec] = std::from_chars(number.data(), last, value);
        if (ec != std::errc{} || ptr != last || !(value > 0.0)) {
            misc_capture_frame::frame_capture.captureFailed(
                "FRAMECAPTURE_INVALID_SIZE", {misc_capture_frame::fmt::fromString(text)});
            return 0;
        }
        (is_scale ? request.scale : request.longest_side) = static_cast<float>(value);
    }
    misc_capture_frame::frame_capture.pending = request;
    misc_capture_frame::updateCaptureHooks();
    return 0;
}

int luaOpenMisc(LuaState* l) {
    static constexpr std::array<lua::LuaLReg, 6> kFuncs = {{
        {.name = "FlashWindow", .func = luaFlashWindow},
        {.name = "IsWindowFocused", .func = luaIsWindowFocused},
        {.name = "FocusWindow", .func = luaFocusWindow},
        {.name = "CopyToClipboard", .func = luaCopyToClipboard},
        {.name = "QueueInteract", .func = luaQueueInteract},
        {.name = "CaptureFrame", .func = luaCaptureFrame},
    }};
    for (const auto& [name, func] : kFuncs) {
        lua::pushCFunction(l, func);
        lua::setGlobal(l, name);
    }
    return 0;
}

void onEnterWorld() {
    lua::registerSlashCommand("INTERACTCMD", "/interact", interactFunctionC);
    CGUIBindings::get()->registerBinding("AWESOME_KEYBIND", "INTERACTIONKEYBIND", "Interaction Button",
        "AWESOME_WOTLK_KEYBINDS", "Awesome Wotlk Keybinds", "QueueInteract()");
    static_cast<void>(framescript::execute{}(misc_capture_frame_lua::kFrameCaptureCommands, "FrameCapture", nullptr));

    if (LuaState* l = lua::getLuaState()) {
        const char* chat_path = game_ui::log_file_names[game_ui::eLogChat];
        std::string chat_msg = std::string("Chat being logged to ") + ((chat_path != nullptr) ? chat_path : "");
        lua::pushString(l, chat_msg.c_str());
        lua::setGlobal(l, "CHATLOGENABLED", true);

        const char* combat_path = game_ui::log_file_names[game_ui::eLogCombat];
        std::string combat_msg = std::string("Combat being logged to ") + ((combat_path != nullptr) ? combat_path : "");
        lua::pushString(l, combat_msg.c_str());
        lua::setGlobal(l, "COMBATLOGENABLED", true);
    }
}

void onLeaveWorld() {
    unit_glows.clear();
    misc_interaction::interact.reset();
    misc_capture_frame::frame_capture.pending = {};
    misc_capture_frame::updateCaptureHooks();
}

void onUpdate() {
    misc_interaction::interact.updateCandidate();
    misc_interaction::interact.processRequest();
    updateSelfHighlight();
    updateUnitGlows();
}
}  // namespace

void misc::initialize(hookkit::HookTransaction& tx) {
    auto& cvars = *extensions::console::kCvarRegistry;
    extensions::console::kLuaLibRegistry->add(luaOpenGlow);

    cvars.add<int>({.name = "interactionAngle", .init = 60, .min{15}, .max{160}});
    cvars.add<int>({.name = "interactionMode", .init = 1, .min{0}, .max{1}});
    cvars.add<int>(
        {.name = "interactionHighlight", .init = 1, .min{0}, .max{1}, .on_change = applyInteractionHighlight});
    cvars.add<int>({.name = "selfHighlight", .init = 0, .min{0}, .max{3}, .on_change = applySelfHighlight});
    cvars.add<int>({.name = "selfHighlightR", .init = 255, .min{0}, .max{255}});
    cvars.add<int>({.name = "selfHighlightG", .init = 140, .min{0}, .max{255}});
    cvars.add<int>({.name = "selfHighlightB", .init = 0, .min{0}, .max{255}});
    cvars.add<int>({.name = "selfHighlightPower", .init = 100, .min{0}, .max{300}});
    cvars.add<int>({.name = "portraitResolution", .init = 64, .min{64}, .max{2048}, .on_change = applyPortraitRes});
    cvars.add<int>({.name = "objectHighlightMode", .init = 0, .min{0}, .max{2}, .on_change = applyObjHlMode});
    cvars.add<int>({.name = "chatLogSessionKey", .init = 1, .min{0}, .max{1}, .on_change = applyChatLogStamp});
    cvars.add<int>({.name = "combatLogSessionKey", .init = 1, .min{0}, .max{1}, .on_change = applyCombatLogStamp});
    cvars.add<float>(
        {.name = "tabTargetDistance", .init = 41.0f, .min{41.0f}, .max{200.0f}, .on_change = applyTabTargetDistance});
    cvars.add<int>({.name = "projectedTextureMode", .init = 1, .min{0}, .max{1}, .on_change = applyProjectedMode});
    cvars.add<int>({.name = "groundTargetClamp", .init = 1, .min{0}, .max{1}, .on_change = applyGroundTargetClamp});

    extensions::console::kLuaLibRegistry->add(luaOpenMisc);

    extensions::framescript::kOnEnter->add(onEnterWorld);
    extensions::framescript::kOnLeave->add(onLeaveWorld);
    extensions::framescript::kOnUpdate->add(onUpdate);

    tx.attach(framescript::secureCmdOptionsParse_hook{}, CGWorldFrame::onLayerTrackTerrain_hook{},
        CSpell_C::cancelPendingAoeTargeting_hook{}, portraitInitialize_site{}, portraitRender_site{});
}
