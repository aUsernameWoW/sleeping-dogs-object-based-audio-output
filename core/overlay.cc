#include "overlay.hh"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

#include "config.hh"
#include "log.hh"
#include "spatial_out.hh"
#include "telemetry.hh"

// Must match ReShade's own Dear ImGui build (see deps/ImGui.props in ReShade).
#define ImTextureID ImU64
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#include <imgui.h>
#include <reshade.hpp>

static_assert(IMGUI_VERSION_NUM == 19250, "ImGui headers must match the ReShade build (6.8.0 uses 1.92.5)");

extern "C" __declspec(dllexport) const char* NAME = "SDAtmos";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
	"Plays Sleeping Dogs through Windows spatial audio (Dolby Atmos, DTS:X): 7.1 bed plus dynamic objects.";

namespace overlay
{
	using telemetry::Reason;

	constexpr float kPi = 3.14159265f;
	constexpr float kDeg = 180.0f / kPi;
	constexpr ULONGLONG kBannerMs = 1500;

	static bool gHudRegistered = false;
	static std::atomic<ULONGLONG> gBannerUntil{ 0 }; // A/B toggle banner, shown even with the HUD off
	static telemetry::Frame gFrame;                  // render thread only

	// ---- Hotkeys ----

	static bool GameIsForeground()
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		return pid == GetCurrentProcessId();
	}

	// Polled on a thread of our own: the game reads keys through Raw Input, and a hotkey must work with or
	// without ReShade.
	static DWORD WINAPI HotkeyThread(void*)
	{
		bool objectsDown = false;
		bool hudDown = false;
		for (;;) {
			Sleep(25);
			const bool foreground = GameIsForeground();
			const bool objectsNow = foreground && gConfig.mToggleObjectsKey && (GetAsyncKeyState(gConfig.mToggleObjectsKey) & 0x8000);
			const bool hudNow = foreground && gConfig.mToggleHudKey && (GetAsyncKeyState(gConfig.mToggleHudKey) & 0x8000);

			if (objectsNow && !objectsDown) {
				const bool on = !gConfig.mObjects.load();
				gConfig.mObjects = on;
				gBannerUntil = GetTickCount64() + kBannerMs;
				LOG("hotkey: dynamic objects %s", on ? "ON" : "OFF (bed only)");
			}
			if (hudNow && !hudDown) {
				const bool on = !gConfig.mHud.load();
				gConfig.mHud = on;
				LOG("hotkey: HUD %s", on ? "on" : "off");
			}
			objectsDown = objectsNow;
			hudDown = hudNow;
		}
	}

	// ---- Drawing helpers ----

	struct Style
	{
		ImU32 mFill;
		ImU32 mOutline;
	};

	static Style StyleFor(const telemetry::Voice& v)
	{
		switch (v.mReason) {
		case Reason::Object: return { IM_COL32(40, 220, 255, 235), IM_COL32(200, 250, 255, 255) };
		case Reason::Candidate: return { IM_COL32(255, 210, 40, 220), IM_COL32(255, 240, 160, 255) };
		case Reason::Spread: return { IM_COL32(150, 150, 150, 150), IM_COL32(200, 200, 200, 180) };
		case Reason::NotMono:
		case Reason::MultiPosition: return { IM_COL32(190, 120, 255, 190), IM_COL32(220, 190, 255, 220) };
		case Reason::Player: return { IM_COL32(90, 230, 120, 200), IM_COL32(190, 255, 200, 230) };
		case Reason::BusFx: return { IM_COL32(255, 140, 60, 200), IM_COL32(255, 200, 150, 230) };
		case Reason::Quiet:
		default: return { IM_COL32(110, 110, 110, 110), IM_COL32(140, 140, 140, 130) };
		}
	}

	static const char* ReasonName(Reason reason)
	{
		switch (reason) {
		case Reason::Object: return "object";
		case Reason::Candidate: return "waiting";
		case Reason::Spread: return "spread";
		case Reason::Quiet: return "quiet";
		case Reason::NotMono: return "not mono";
		case Reason::MultiPosition: return "multi-pos";
		case Reason::Player: return "player";
		case Reason::BusFx: return "bus fx";
		default: return "?";
		}
	}

	static float LevelDb(float level)
	{
		return level > 1e-6f ? 20.0f * std::log10(level) : -120.0f;
	}

	// Dot radius from level: -70 dB → small, -10 dB → big.
	static float DotRadius(float level, float scale)
	{
		const float t = std::clamp((LevelDb(level) + 70.0f) / 60.0f, 0.0f, 1.0f);
		return (2.5f + 6.5f * t) * scale;
	}

	static bool ShouldDraw(const telemetry::Voice& v)
	{
		return v.mReason == Reason::Object || gConfig.mHudBedVoices.load();
	}

	// Elevation cue: a stick up or down from the dot, length ∝ phi.
	static void DrawElevation(ImDrawList* draw, ImVec2 p, float phi, float radius, ImU32 color, float scale)
	{
		const float deg = phi * kDeg;
		if (std::fabs(deg) < 8.0f) {
			return;
		}
		const float length = (6.0f + 14.0f * std::min(std::fabs(deg), 90.0f) / 90.0f) * scale;
		const float dir = deg > 0.0f ? -1.0f : 1.0f; // screen y grows downwards
		const ImVec2 tip(p.x, p.y + dir * (radius + length));
		draw->AddLine(ImVec2(p.x, p.y + dir * radius), tip, color, 1.5f * scale);
		const float w = 3.5f * scale;
		draw->AddTriangleFilled(ImVec2(tip.x, tip.y + dir * w), ImVec2(tip.x - w, tip.y), ImVec2(tip.x + w, tip.y), color);
	}

	static void DrawRadar(ImDrawList* draw, const ImVec2& display, float scale, float hfov)
	{
		const float radius = 150.0f * scale;
		const ImVec2 c(display.x - radius - 24.0f * scale, radius + 24.0f * scale);
		const float range = gConfig.mHudRadarRange.load();
		auto mapDistance = [&](float r) { return radius * std::min(1.0f, std::log1p(std::max(r, 0.0f)) / std::log1p(range)); };

		draw->AddCircleFilled(c, radius, IM_COL32(0, 0, 0, 120), 64);
		draw->AddCircle(c, radius, IM_COL32(255, 255, 255, 90), 64, 1.0f);
		char text[32];
		for (float ring : { 3.0f, 10.0f, 30.0f }) {
			if (ring >= range) {
				continue;
			}
			const float rr = mapDistance(ring);
			draw->AddCircle(c, rr, IM_COL32(255, 255, 255, 40), 48, 1.0f);
			snprintf(text, sizeof(text), "%.0fm", ring);
			draw->AddText(ImVec2(c.x + 2.0f, c.y - rr), IM_COL32(255, 255, 255, 90), text);
		}
		// Camera view cone.
		for (float side : { -1.0f, 1.0f }) {
			const float a = side * hfov * 0.5f;
			draw->AddLine(c, ImVec2(c.x + std::sin(a) * radius, c.y - std::cos(a) * radius), IM_COL32(255, 255, 255, 60), 1.0f);
		}
		draw->AddCircleFilled(c, 3.0f * scale, IM_COL32(255, 255, 255, 200), 12);

		// Quiet ones first so objects end up on top.
		const telemetry::Voice* order[telemetry::kMaxVoices];
		uint32_t count = 0;
		for (uint32_t i = 0; i < gFrame.mVoiceCount; ++i) {
			if (ShouldDraw(gFrame.mVoices[i])) {
				order[count++] = &gFrame.mVoices[i];
			}
		}
		std::sort(order, order + count, [](const telemetry::Voice* a, const telemetry::Voice* b) {
			return (a->mReason == Reason::Object) != (b->mReason == Reason::Object) ? b->mReason == Reason::Object : a->mLevel < b->mLevel;
		});

		for (uint32_t i = 0; i < count; ++i) {
			const telemetry::Voice& v = *order[i];
			const float rr = mapDistance(v.mDistance);
			const ImVec2 p(c.x + std::sin(v.mTheta) * rr, c.y - std::cos(v.mTheta) * rr);
			const Style style = StyleFor(v);
			const float dot = DotRadius(v.mLevel, scale);
			draw->AddCircleFilled(p, dot, style.mFill, 16);
			DrawElevation(draw, p, v.mPhi, dot, style.mOutline, scale);
			if (v.mReason == Reason::Object) {
				snprintf(text, sizeof(text), "%d", v.mSlot);
				draw->AddText(ImVec2(p.x + dot + 1.0f, p.y - dot - 2.0f), style.mOutline, text);
			}
		}

		// Status under the radar.
		spatial::Status status;
		spatial::GetStatus(status);
		uint32_t objects = 0;
		for (uint32_t i = 0; i < gFrame.mVoiceCount; ++i) {
			objects += gFrame.mVoices[i].mReason == Reason::Object;
		}
		char line[160];
		const bool on = gConfig.mObjects.load();
		snprintf(line, sizeof(line), "SDAtmos  objects %s  %u/%u  voices %u (+%u 2D)  latency %.0f ms%s",
			on ? "ON" : "OFF", objects, gFrame.mObjectsTarget, gFrame.mVoiceCount, gFrame.mUnpositioned,
			status.mFill / 48.0f, status.mActive ? "" : "  [spatial stream DOWN]");
		const ImVec2 size = ImGui::CalcTextSize(line, nullptr, false, -1.0f);
		const ImVec2 pos(display.x - size.x - 24.0f * scale, c.y + radius + 8.0f * scale);
		draw->AddRectFilled(ImVec2(pos.x - 4, pos.y - 2), ImVec2(pos.x + size.x + 4, pos.y + size.y + 2), IM_COL32(0, 0, 0, 140), 3.0f, 0);
		draw->AddText(pos, status.mActive ? IM_COL32(255, 255, 255, 230) : IM_COL32(255, 90, 90, 255), line);
	}

	static void DrawMarkers(ImDrawList* draw, const ImVec2& display, float scale, float tanH, float tanV)
	{
		const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
		const bool labels = gConfig.mHudLabels.load();
		char text[96];
		for (uint32_t i = 0; i < gFrame.mVoiceCount; ++i) {
			const telemetry::Voice& v = gFrame.mVoices[i];
			if (!ShouldDraw(v)) {
				continue;
			}
			// Camera space: x right, y up, z forward.
			const float x = std::sin(v.mTheta) * std::cos(v.mPhi);
			const float y = std::sin(v.mPhi);
			const float z = std::cos(v.mTheta) * std::cos(v.mPhi);
			if (z < 0.05f) {
				continue;
			}
			const ImVec2 p(c.x + (x / z) / tanH * c.x, c.y - (y / z) / tanV * c.y);
			if (p.x < 0.0f || p.y < 0.0f || p.x > display.x || p.y > display.y) {
				continue;
			}
			const Style style = StyleFor(v);
			const float dot = DotRadius(v.mLevel, scale) * 1.4f;
			if (v.mReason == Reason::Object) {
				draw->AddCircle(p, dot + 4.0f * scale, style.mOutline, 24, 2.0f * scale);
				draw->AddCircleFilled(p, dot * 0.6f, style.mFill, 16);
			}
			else {
				draw->AddCircle(p, dot, style.mFill, 16, 1.5f * scale);
			}
			if (labels || v.mReason == Reason::Object) {
				char role[16];
				if (v.mReason == Reason::Object) {
					snprintf(role, sizeof(role), labels ? "obj%d" : "%d", v.mSlot);
				}
				else {
					snprintf(role, sizeof(role), "%s", ReasonName(v.mReason));
				}
				if (labels) {
					snprintf(text, sizeof(text), "%s %.0fdB %.1fm snd %u", role, LevelDb(v.mLevel), v.mDistance, v.mSoundID);
				}
				else {
					snprintf(text, sizeof(text), "%s", role);
				}
				draw->AddText(ImVec2(p.x + dot + 5.0f * scale, p.y - 7.0f * scale), style.mOutline, text);
			}
		}
	}

	static void DrawBanner(ImDrawList* draw, const ImVec2& display, float scale)
	{
		const bool on = gConfig.mObjects.load();
		const char* text = on ? "SDAtmos: dynamic objects ON" : "SDAtmos: objects OFF (7.1 bed only)";
		ImFont* font = ImGui::GetFont();
		// ImFont::CalcTextSizeA isn't in ReShade's function table; text width scales linearly with font size.
		const float size = ImGui::GetFontSize() * 2.0f;
		const ImVec2 base = ImGui::CalcTextSize(text, nullptr, false, -1.0f);
		const ImVec2 extent(base.x * 2.0f, base.y * 2.0f);
		const ImVec2 pos((display.x - extent.x) * 0.5f, display.y * 0.12f);
		draw->AddRectFilled(ImVec2(pos.x - 12 * scale, pos.y - 6 * scale), ImVec2(pos.x + extent.x + 12 * scale, pos.y + extent.y + 6 * scale),
			IM_COL32(0, 0, 0, 170), 6.0f * scale, 0);
		draw->AddText(font, size, pos, on ? IM_COL32(40, 220, 255, 255) : IM_COL32(255, 210, 40, 255), text, nullptr, 0.0f, nullptr);
	}

	static void OnHud(reshade::api::effect_runtime*)
	{
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		if (display.x <= 0.0f || display.y <= 0.0f) {
			return;
		}
		ImDrawList* draw = ImGui::GetBackgroundDrawList(static_cast<ImGuiViewport*>(nullptr));
		const float scale = display.y / 1080.0f;

		if (gConfig.mHud.load() && telemetry::Read(gFrame)) {
			const float tanV = std::tan(gConfig.mHudFov.load() * 0.5f / kDeg);
			const float tanH = tanV * display.x / display.y;
			if (gConfig.mHudMarkers.load()) {
				DrawMarkers(draw, display, scale, tanH, tanV);
			}
			if (gConfig.mHudRadar.load()) {
				DrawRadar(draw, display, scale, 2.0f * std::atan(tanH));
			}
		}
		if (GetTickCount64() < gBannerUntil.load()) {
			DrawBanner(draw, display, scale);
		}
	}

	// 'reshade_overlay' makes ReShade run its ImGui pass every frame, so it's only registered while there is
	// something to draw.
	static void OnPresent(reshade::api::effect_runtime*)
	{
		const bool want = gConfig.mHud.load() || GetTickCount64() < gBannerUntil.load();
		if (want == gHudRegistered) {
			return;
		}
		gHudRegistered = want;
		if (want) {
			reshade::register_event<reshade::addon_event::reshade_overlay>(OnHud);
		}
		else {
			reshade::unregister_event<reshade::addon_event::reshade_overlay>(OnHud);
		}
	}

	// ---- ReShade menu tab ----

	static void Checkbox(const char* label, std::atomic<bool>& value)
	{
		bool v = value.load();
		if (ImGui::Checkbox(label, &v)) {
			value = v;
		}
	}

	static void OnMenu(reshade::api::effect_runtime*)
	{
		spatial::Status status;
		spatial::GetStatus(status);

		if (status.mActive) {
			ImGui::Text("Spatial stream: %s", status.mEndpoint);
			ImGui::Text("Bed [%s], %u dynamic objects reserved (format allows %u)", status.mBed, status.mSlots, status.mFormatMax);
			ImGui::Text("Latency %.0f ms | objects sounding %u, held %u | underruns %llu | failed %llu | folded %llu",
				status.mFill / 48.0f, status.mSounding, status.mHeld, status.mUnderruns, status.mActivationFailures, status.mFoldedPasses);
		}
		else {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Spatial stream not running: the game plays through XAudio2 (see SDAtmos.log).");
		}

		ImGui::Separator();
		Checkbox("Dynamic objects (A/B hotkey: F9 by default)", gConfig.mObjects);
		int maxObjects = gConfig.mMaxObjects.load();
		if (ImGui::SliderInt("Max objects", &maxObjects, 0, static_cast<int>(status.mSlots ? status.mSlots : spatial::kMaxObjects), "%d", 0)) {
			gConfig.mMaxObjects = maxObjects;
		}
		float distance = gConfig.mObjectDistance.load();
		if (ImGui::SliderFloat("Object distance (m)", &distance, 0.5f, 10.0f, "%.1f", 0)) {
			gConfig.mObjectDistance = distance;
		}
		Checkbox("Player's own sounds stay in the bed (footsteps, foley)", gConfig.mPlayerInBed);
		float lift = gConfig.mActorLift.load();
		if (ImGui::SliderFloat("Lift characters' sounds off the ground (m)", &lift, 0.0f, 3.0f, "%.1f", 0)) {
			gConfig.mActorLift = lift;
		}
		int busFx = gConfig.mBusFx.load();
		if (ImGui::Combo("Buses with effects other than EQ", &busFx, "ignore\0voices stay in the bed (master bus excepted)\0same, master bus included\0", -1)) {
			gConfig.mBusFx = busFx;
		}

		ImGui::Separator();
		Checkbox("HUD (F8 by default)", gConfig.mHud);
		Checkbox("Radar", gConfig.mHudRadar);
		ImGui::SameLine(0.0f, -1.0f);
		Checkbox("On-screen markers", gConfig.mHudMarkers);
		ImGui::SameLine(0.0f, -1.0f);
		Checkbox("Labels", gConfig.mHudLabels);
		ImGui::SameLine(0.0f, -1.0f);
		Checkbox("Show bed voices", gConfig.mHudBedVoices);
		float fov = gConfig.mHudFov.load();
		if (ImGui::SliderFloat("Marker FOV (vertical, deg)", &fov, 20.0f, 120.0f, "%.0f", 0)) {
			gConfig.mHudFov = fov;
		}
		float range = gConfig.mHudRadarRange.load();
		if (ImGui::SliderFloat("Radar range (m)", &range, 5.0f, 300.0f, "%.0f", 0)) {
			gConfig.mHudRadarRange = range;
		}
		ImGui::TextDisabled("Cyan = object, yellow = qualifies but waiting, gray = spread/quiet (bed), purple = not mono / several positions, green = player (bed), orange = bus has effects (bed).");
		ImGui::TextDisabled("Stick up/down = elevation. Radar distance is logarithmic.");

		if (ImGui::Button("Save to SDAtmos.ini", ImVec2(0.0f, 0.0f))) {
			config::Save();
			LOG("menu: settings saved");
		}

		if (ImGui::CollapsingHeader("Voices", 0) && telemetry::Read(gFrame)) {
			const telemetry::Voice* order[telemetry::kMaxVoices];
			for (uint32_t i = 0; i < gFrame.mVoiceCount; ++i) {
				order[i] = &gFrame.mVoices[i];
			}
			std::sort(order, order + gFrame.mVoiceCount, [](const telemetry::Voice* a, const telemetry::Voice* b) { return a->mLevel > b->mLevel; });

			ImGui::Text("%u positioned voices, %u without position (2D)", gFrame.mVoiceCount, gFrame.mUnpositioned);
			if (ImGui::BeginTable("voices", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit, ImVec2(0.0f, 0.0f), 0.0f)) {
				ImGui::TableSetupColumn("Role", 0, 0.0f, 0);
				ImGui::TableSetupColumn("Azimuth", 0, 0.0f, 0);
				ImGui::TableSetupColumn("Elevation", 0, 0.0f, 0);
				ImGui::TableSetupColumn("Distance", 0, 0.0f, 0);
				ImGui::TableSetupColumn("Level", 0, 0.0f, 0);
				ImGui::TableSetupColumn("Sound ID", 0, 0.0f, 0);
				ImGui::TableSetupColumn("Slot", 0, 0.0f, 0);
				ImGui::TableHeadersRow();
				for (uint32_t i = 0; i < gFrame.mVoiceCount; ++i) {
					const telemetry::Voice& v = *order[i];
					ImGui::TableNextRow(0, 0.0f);
					ImGui::TableSetColumnIndex(0);
					ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(StyleFor(v).mOutline), "%s", ReasonName(v.mReason));
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%+.0f", v.mTheta * kDeg);
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%+.0f", v.mPhi * kDeg);
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%.1f m", v.mDistance);
					ImGui::TableSetColumnIndex(4);
					ImGui::Text("%.0f dB", LevelDb(v.mLevel));
					ImGui::TableSetColumnIndex(5);
					ImGui::Text("%u", v.mSoundID);
					ImGui::TableSetColumnIndex(6);
					if (v.mSlot >= 0) {
						ImGui::Text("%d", v.mSlot);
					}
				}
				ImGui::EndTable();
			}
		}
	}

	void Install(HMODULE self)
	{
		HANDLE thread = CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
		if (thread) {
			CloseHandle(thread);
		}
		LOG("overlay: hotkeys 0x%X = objects on/off, 0x%X = HUD", gConfig.mToggleObjectsKey, gConfig.mToggleHudKey);

		// ReShade (dxgi.dll) is a static import of the exe, so it's loaded before the ASI loader runs us.
		if (!reshade::register_addon(self)) {
			LOG("overlay: ReShade not found (or incompatible ImGui), no menu/HUD");
			return;
		}
		reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
		reshade::register_overlay("SDAtmos", OnMenu);
		LOG("overlay: registered as ReShade add-on (menu tab + HUD)");
	}
}
