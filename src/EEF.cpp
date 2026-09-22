#include "PCH.h"

#include <SimpleIni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "REL/Offset2ID.h"
#include "REL/Relocation.h"
#include "SKSE/Trampoline.h"

#include <hde64.h>

#include "EEF.h"

namespace EEF
{
	namespace
	{
		bool s_onEquip{ true };
		bool s_onActorLoad{ true };
		bool s_recalcWeightOnLoad{ false };
		bool s_redirectDispel{ true };
		bool s_scriptEquipEventFix{ false };

		constexpr auto kIniPath = "Data\\SKSE\\Plugins\\EquipEnchantmentFix.ini";

		std::mutex s_queueLock;

		// Actors already queued for a re-check, so a burst of events for the same
		// actor (load, equip, effect removal) collapses into a single task.
		std::unordered_set<
			RE::ObjectRefHandle,
			RE::BSCRC32<RE::ObjectRefHandle>>
			s_pending;

		// True while a drain task is already queued, so a burst of events only
		// ever queues one.
		bool s_drainQueued{ false };

		// Form-table lookups are not safe from every context, so every lookup
		// we cannot remove goes through here: a bad table turns into a missing
		// form instead of a crash. The per-event cost is microseconds (x64 SEH
		// is table-driven and free when nothing raises), which is what makes
		// handling thousands of load-time events affordable.
		[[nodiscard]] RE::TESForm* LookupFormSafe(RE::FormID a_id)
		{
			if (!a_id) {
				return nullptr;
			}

			__try {
				return RE::TESForm::LookupByID(a_id);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				SKSE::log::error("TESForm::LookupByID({:08X}) raised; treating as missing", a_id);
				return nullptr;
			}
		}

		template <class T>
		[[nodiscard]] T* LookupFormSafe(RE::FormID a_id)
		{
			auto* form = LookupFormSafe(a_id);
			return form ? form->As<T>() : nullptr;
		}

		[[nodiscard]] bool IsValidActor(RE::Actor* a_actor)
		{
			// IsDeleted() / IsInitialized() read formFlags straight off the
			// object (non-virtual). IsDead() below is virtual, but by the time
			// it runs the pointer has already been validated by the caller
			// (handle or live-form-table lookup), so this is safe.
			return a_actor && !a_actor->IsDeleted() && a_actor->IsInitialized() && !a_actor->IsDead();
		}

		// The instance enchantment actually carried by a worn item's extra data.
		// Only ExtraEnchantment counts (no base-form fallback), matching the
		// original plugin -- otherwise the ability check can never match and we
		// re-apply on every equip.
		[[nodiscard]] RE::EnchantmentItem* GetWornEnchantment(RE::ExtraDataList* a_worn)
		{
			if (!a_worn) {
				return nullptr;
			}
			if (auto* extraEnch = a_worn->GetByType<RE::ExtraEnchantment>()) {
				return extraEnch->enchantment;
			}
			return nullptr;
		}

		// A worn armour can only carry a constant-effect enchantment
		// (CastingType::kConstantEffect). Anything else sitting on it is a
		// foreign configuration -- most commonly a weapon enchantment moved
		// onto armour by a "no enchantment restriction" mod. Feeding that to
		// UpdateArmorAbility drives the engine down a branch it never expects
		// and crashes (null function-pointer call). Skip it rather than
		// trying to "restore" it.
		[[nodiscard]] bool IsValidArmorEnchantment(RE::EnchantmentItem* a_enchantment)
		{
			if (!a_enchantment) {
				return false;
			}
			return a_enchantment->data.castingType == RE::MagicSystem::CastingType::kConstantEffect;
		}

		// Result of "is this item's enchantment already on the actor". The third
		// state matters: the walk can fault, and a fault is NOT "absent".
		// Collapsing it to absent makes every check re-apply the effect, which is
		// exactly how effects ended up stacking -- so a fault is reported
		// separately and the caller skips the item instead of duplicating it.
		enum class AbilityCheck : std::uint8_t
		{
			kMissing,  // no matching (source, spell) -> safe to apply
			kPresent,  // the actor already carries it
			kUnknown   // the walk faulted -> we cannot tell
		};

		std::atomic<std::uint32_t> s_abilityQueryFaults{ 0 };

		// "Is this enchantment already applied" the way the original plugin
		// answered it: walk the actor's active effects and match the (source,
		// spell) pair, where source is the item and spell is its enchantment.
		//
		// Actor::HasMagicEffect looked like the portable equivalent, but it faults
		// on every runtime tested (SE 1.5.97 included), which silently disabled
		// the check. MagicTarget::GetActiveEffectList is a RelocateVirtual
		// vtable-slot forwarder, so it must be reached through a correctly-offset
		// MagicTarget* -- that is what AsMagicTarget() yields; a bare Actor* would
		// read the wrong vtable.
		//
		// The walk lives outside the __try because MSVC refuses __try in a
		// function that needs object unwinding, and the range-for iterator is
		// such an object. A fault raised inside the call still unwinds into the
		// caller's handler.
		struct AbilityQuery
		{
			const void* source;
			const void* spell;
			bool        found;
		};

		[[nodiscard]] bool WalkForAbility(RE::MagicTarget* a_target, AbilityQuery* a_query)
		{
			auto* list = a_target->GetActiveEffectList();
			if (!list) {
				return false;
			}

			for (auto& effect : *list) {
				if (!effect || effect->source != a_query->source || effect->spell != a_query->spell) {
					continue;
				}
				// Flagged means finished at the actor's next update, which a paused
				// menu holds off. The engine's re-equip unequips first and re-applies
				// over the flagged copy, so counting it would block that re-apply.
				if (effect->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
					continue;
				}
				a_query->found = true;
				return true;
			}
			return false;
		}

		[[nodiscard]] AbilityCheck TryHasItemAbility(RE::Actor* a_actor, RE::TESForm* a_form, RE::EnchantmentItem* a_enchantment)
		{
			if (!a_actor || !a_form || !a_enchantment) {
				return AbilityCheck::kMissing;
			}

			auto* target = a_actor->AsMagicTarget();
			if (!target) {
				return AbilityCheck::kUnknown;
			}

			AbilityQuery query{ a_form, a_enchantment, false };

			__try {
				(void)WalkForAbility(target, &query);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				// Log the first fault only: a broken walk faults on every call
				// and would otherwise flood the log.
				if (s_abilityQueryFaults.fetch_add(1, std::memory_order_relaxed) == 0) {
					SKSE::log::error(
						"active-effect walk faulted (actor {:08X}, item {:08X}); ability checks are "
						"treated as unknown from now on, so nothing will be re-applied (a missed "
						"re-apply is harmless, a duplicate is not).",
						a_actor->GetFormID(),
						a_form->GetFormID());
				}
				return AbilityCheck::kUnknown;
			}

			return query.found ? AbilityCheck::kPresent : AbilityCheck::kMissing;
		}

		[[nodiscard]] bool TryUpdateArmorAbility(RE::Actor* a_actor, RE::TESForm* a_form, RE::ExtraDataList* a_extraData)
		{
			__try {
				a_actor->UpdateArmorAbility(a_form, a_extraData);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		[[nodiscard]] bool HasItemAbility(RE::Actor* a_actor, RE::TESForm* a_form, RE::EnchantmentItem* a_enchantment)
		{
			switch (TryHasItemAbility(a_actor, a_form, a_enchantment)) {
			case AbilityCheck::kPresent:
				return true;
			case AbilityCheck::kUnknown:
				// The walk is not trustworthy on this runtime. Report "already
				// present" so ProcessActor does not re-apply: skipping is the
				// safe failure mode, duplicating is not.
				return true;
			case AbilityCheck::kMissing:
			default:
				return false;
			}
		}

		// --- engine hook -------------------------------------------------------
		// The engine applies a worn item's enchantment through
		// Actor::UpdateArmorAbility and does not de-duplicate: calling it a second
		// time for the same item stacks the effect. The original plugin hooked the
		// engine's call to it to block that before it happened; this hooks the same
		// call site, using the exact (source, spell) check ProcessActor uses, so the
		// block and the repair agree on what "already applied" means.
		//
		// Note the hook sits on the ENGINE's call site, not on the function: our own
		// re-apply calls the function directly and therefore still goes through,
		// which is what lets a genuinely missing enchantment be restored.
		using UpdateArmorAbility_t = void (*)(RE::Actor*, RE::TESForm*, RE::ExtraDataList*);
		UpdateArmorAbility_t UpdateArmorAbility_orig{ nullptr };

		// The engine's order (its own picker, 1.6.1170): an item carrying both
		// gets the record's, so matching the instance's would never find the
		// applied effect.
		[[nodiscard]] RE::EnchantmentItem* GetApplicableEnchantment(RE::TESForm* a_form, RE::ExtraDataList* a_extraData)
		{
			if (a_form) {
				if (auto* enchantable = a_form->As<RE::TESEnchantableForm>(); enchantable && enchantable->formEnchanting) {
					return enchantable->formEnchanting;
				}
			}
			return GetWornEnchantment(a_extraData);
		}

		void UpdateArmorAbility_Hook(RE::Actor* a_actor, RE::TESForm* a_form, RE::ExtraDataList* a_extraData)
		{
			if (a_actor && a_form && a_form->As<RE::TESObjectARMO>()) {
				auto* enchantment = GetApplicableEnchantment(a_form, a_extraData);
				if (enchantment && IsValidArmorEnchantment(enchantment) &&
					HasItemAbility(a_actor, a_form, enchantment)) {
					// Already on the actor. Letting the engine run would stack it.
					return;
				}
			}

			UpdateArmorAbility_orig(a_actor, a_form, a_extraData);
		}

		// The engine reaches UpdateArmorAbility through a five-byte `call rel32`
		// at a known offset inside its caller. That CALL SITE is what gets hooked,
		// not the function itself: SKSE::Trampoline::write_call replaces exactly
		// that instruction and returns the call's original target, so the real
		// function stays untouched and callable. Patching the function entry
		// instead corrupts it -- the trampoline does not preserve the prologue, and
		// the "original" it hands back is computed from the prologue bytes.
		template <class Fn>
		[[nodiscard]] bool InstallCallHook(const char* a_what, std::uintptr_t a_callSite, Fn a_hook, Fn& a_orig)
		{
			if (!a_callSite) {
				SKSE::log::error("{} call site could not be resolved", a_what);
				return false;
			}

			try {
				a_orig = reinterpret_cast<Fn>(SKSE::GetTrampoline().write_call<5>(a_callSite, a_hook));
			} catch (const std::exception& e) {
				SKSE::log::error("{} hook failed: {}", a_what, e.what());
				return false;
			} catch (...) {
				SKSE::log::error("{} hook failed (unknown exception)", a_what);
				return false;
			}

			if (!a_orig) {
				SKSE::log::error("{} hook produced a null original pointer", a_what);
				return false;
			}

			SKSE::log::info("{} hook installed at {:X}", a_what, a_callSite);
			return true;
		}

		// --- finding the call sites at launch ----------------------------------
		// A call site is a function start plus an offset into its body. The
		// Address Library keeps the start stable across versions; nothing keeps
		// the offset. Given the callee's id as well, the offset is derivable: the
		// one call inside the caller that lands on the callee. So it is derived
		// at launch, and verified on the machine it runs on, instead of carried
		// per version.
		[[nodiscard]] std::uintptr_t EndOfFunction(std::uintptr_t a_start)
		{
			// Sorted by offset; built once.
			static const REL::Offset2ID table;
			const auto base = REL::Module::get().base();
			const auto offset = a_start - base;
			const auto next = std::upper_bound(table.begin(), table.end(), offset, [](std::uint64_t a_lhs, const REL::IDDB::mapping_t& a_rhs) {
				return a_lhs < a_rhs.offset;
			});
			return next != table.end() ? base + next->offset : a_start + 0x1000;
		}

		[[nodiscard]] std::vector<std::uintptr_t> CallsTo(std::uintptr_t a_begin, std::uintptr_t a_end, std::uintptr_t a_callee)
		{
			std::vector<std::uintptr_t> sites;
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_begin);
			for (std::size_t i = 0; i + 5 <= a_end - a_begin; ++i) {
				if (bytes[i] != 0xE8) {
					continue;
				}
				std::int32_t rel = 0;
				std::memcpy(&rel, bytes + i + 1, sizeof(rel));
				if (a_begin + i + 5 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel)) == a_callee) {
					sites.push_back(a_begin + i);
				}
			}
			return sites;
		}

		[[nodiscard]] std::uintptr_t FindCallSite(const char* a_what, std::uintptr_t a_caller, std::uintptr_t a_callee)
		{
			const auto end = EndOfFunction(a_caller);
			const auto sites = CallsTo(a_caller, end, a_callee);
			if (sites.size() != 1) {
				SKSE::log::error("{}: {} call(s) to the callee inside the caller at {:X} (extent {:#x}); not hooking",
					a_what, sites.size(), a_caller, end - a_caller);
				return 0;
			}
			SKSE::log::info("{}: call site found at {:X} (caller +{:#x})", a_what, sites[0], sites[0] - a_caller);
			return sites[0];
		}

		[[nodiscard]] bool InstallUpdateArmorAbilityHook()
		{
			static REL::Relocation<std::uintptr_t> caller{ REL::RelocationID(36976, 38001) };
			static REL::Relocation<std::uintptr_t> callee{ REL::RelocationID(37802, 38751) };
			const auto site = FindCallSite("UpdateArmorAbility", caller.address(), callee.address());
			return site && InstallCallHook("UpdateArmorAbility", site, &UpdateArmorAbility_Hook, UpdateArmorAbility_orig);
		}

		// --- the dispel redirect ----------------------------------------------
		// The transfer routine dispels every worn enchantment on the NPC and then
		// asks for a model update, which is what re-applies them and which only
		// rebuilds when an equipment change flagged it. A potion flags nothing.
		// No listener can react either: the engine raises the apply/remove event
		// only for effects with a unique id, which it assigns only to (by all
		// appearances) scripted effects. So what is still worn must not be
		// dispelled in the first place, at both sites 1.3.5 redirected: the
		// transfer routine's own call, and the rebuild helper's that its model
		// update reaches when equipment did change (traced on 1.6.1170). The
		// UpdateArmorAbility block above is the other half: the armour trade
		// still re-equips, and would stack a copy of what was kept.
		using DispelWornItemEnchantments_t = void (*)(RE::Actor*);
		DispelWornItemEnchantments_t DispelWornItemEnchantments_orig{ nullptr };

		// An active effect names its source as the base object only, so (item,
		// enchantment) is the finest identity there is; on the item alone, any
		// worn copy would keep an effect it did not produce.
		struct WornEnchantment
		{
			RE::TESBoundObject* source;
			RE::MagicItem*      spell;
		};

		struct StaleQuery
		{
			const std::vector<WornEnchantment>* worn;
			std::vector<RE::ActiveEffect*>*     stale;
		};

		// Weapons are left alone as the engine's visitor leaves them; the unequip
		// path handles those. Split out so the walk can sit under __try, which
		// MSVC refuses beside objects with destructors.
		void CollectStaleEffects(RE::MagicTarget* a_target, StaleQuery* a_query)
		{
			auto* list = a_target->GetActiveEffectList();
			if (!list) {
				return;
			}
			for (auto* effect : *list) {
				if (!effect || !effect->source || !effect->spell) {
					continue;
				}
				if (effect->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
					continue;
				}
				if (!effect->source->As<RE::TESObjectARMO>()) {
					continue;
				}
				const auto stillWorn = std::any_of(a_query->worn->begin(), a_query->worn->end(), [&](const WornEnchantment& w) {
					return w.source == effect->source && w.spell == effect->spell;
				});
				if (!stillWorn) {
					a_query->stale->push_back(effect);
				}
			}
		}

		[[nodiscard]] bool TryCollectStaleEffects(RE::MagicTarget* a_target, StaleQuery* a_query)
		{
			__try {
				CollectStaleEffects(a_target, a_query);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void DispelWornItemEnchantments_Hook(RE::Actor* a_actor)
		{
			if (!IsValidActor(a_actor)) {
				DispelWornItemEnchantments_orig(a_actor);
				return;
			}

			auto* changes = a_actor->GetInventoryChanges(true);
			if (!changes || !changes->entryList) {
				DispelWornItemEnchantments_orig(a_actor);
				return;
			}

			// One base item can be in the bag several times, each instance with its
			// own enchantment; only the worn instance's counts.
			std::vector<WornEnchantment> worn;
			for (auto* entry : *changes->entryList) {
				if (!entry || !entry->object || !entry->object->As<RE::TESObjectARMO>() || !entry->extraLists) {
					continue;
				}
				for (auto* xList : *entry->extraLists) {
					if (!xList || !(xList->HasType<RE::ExtraWorn>() || xList->HasType<RE::ExtraWornLeft>())) {
						continue;
					}
					if (auto* enchantment = GetApplicableEnchantment(entry->object, xList)) {
						worn.push_back({ entry->object, enchantment });
					}
				}
			}

			auto* target = a_actor->AsMagicTarget();
			if (!target) {
				DispelWornItemEnchantments_orig(a_actor);
				return;
			}

			std::vector<RE::ActiveEffect*> stale;
			StaleQuery                     query{ &worn, &stale };
			if (!TryCollectStaleEffects(target, &query)) {
				// The engine's own dispel is the known-safe fallback.
				SKSE::log::error("active-effect walk faulted in the dispel redirect (actor {:08X}); engine dispel used", a_actor->GetFormID());
				DispelWornItemEnchantments_orig(a_actor);
				return;
			}

			for (auto* effect : stale) {
				effect->Dispel(false);
			}

			SKSE::log::debug("dispel redirect: actor {:08X}, {} worn armour kept, {} stale effect(s) dispelled",
				a_actor->GetFormID(), worn.size(), stale.size());
		}

		// The helper was renumbered at 1.6.629 (24738 became 418622) and an
		// absent-id lookup is fatal, so on AE it is found by elimination: the
		// callers of the dispel that are neither the transfer routine nor the
		// dispel-and-recast routine, exactly one on 1.6.1170. SE keeps 1.3.5's id.
		[[nodiscard]] std::uintptr_t FindRebuildDispelSite(std::uintptr_t a_callee, std::uintptr_t a_transferSite)
		{
			if (REL::Module::IsSE()) {
				static REL::Relocation<std::uintptr_t> rebuild{ REL::ID(24234) };
				return FindCallSite("DispelWornItemEnchantments (model rebuild)", rebuild.address(), a_callee);
			}

			// Never called; located only so its own call to the dispel can be
			// excluded from the search. It re-applies for itself, so that call
			// must keep dispelling. The five-argument dispel-and-recast routine,
			// not CommonLib's CastPermanentMagic (38753, its neighbour).
			static REL::Relocation<std::uintptr_t> recast{ REL::ID(38754) };
			const auto recastBegin = recast.address();
			const auto recastEnd = EndOfFunction(recastBegin);

			const auto text = REL::Module::get().segment(REL::Segment::textx);
			auto sites = CallsTo(text.address(), text.address() + text.size(), a_callee);
			std::erase_if(sites, [&](std::uintptr_t a_site) {
				return a_site == a_transferSite || (a_site >= recastBegin && a_site < recastEnd);
			});
			if (sites.size() != 1) {
				SKSE::log::error("DispelWornItemEnchantments (model rebuild): {} other caller(s) of the dispel in the code segment, expected 1; not hooking", sites.size());
				return 0;
			}
			SKSE::log::info("DispelWornItemEnchantments (model rebuild): call site found at {:X}", sites[0]);
			return sites[0];
		}

		// The same callee at both sites, so one original serves.
		[[nodiscard]] bool InstallDispelRedirect()
		{
			static REL::Relocation<std::uintptr_t> callee{ REL::RelocationID(33828, 34620) };
			static REL::Relocation<std::uintptr_t> transfer{ REL::RelocationID(50212, 51141) };

			const auto transferSite = FindCallSite("DispelWornItemEnchantments (container transfer)", transfer.address(), callee.address());
			if (!transferSite || !InstallCallHook("DispelWornItemEnchantments (container transfer)", transferSite, &DispelWornItemEnchantments_Hook, DispelWornItemEnchantments_orig)) {
				return false;
			}

			const auto rebuildSite = FindRebuildDispelSite(callee.address(), transferSite);
			if (!rebuildSite) {
				SKSE::log::warn("DispelWornItemEnchantments (model rebuild): not hooked; an armour trade will still drop the other worn enchantments until the menu closes");
				return true;
			}
			DispelWornItemEnchantments_t rebuildOrig{ nullptr };
			if (InstallCallHook("DispelWornItemEnchantments (model rebuild)", rebuildSite, &DispelWornItemEnchantments_Hook, rebuildOrig)) {
				DispelWornItemEnchantments_orig = rebuildOrig;
			}
			return true;
		}

		// --- the script-equip fix (optional) --------------------------------
		// Papyrus `Actor.EquipItem` reaches the engine with no extra data, so the
		// engine equips a bare instance of the form and the item's OnEquipped
		// event never fires. Mods that script an equip depend on that event --
		// Torch Mechanics Fixed names this exact option as a requirement, the
		// Aetherial Crown scripted-equip fix exists only because of it, and
		// follower outfit scripts are the same shape. The original plugin's
		// ScriptEquipEventFix (off by default there too) answers it by putting
		// the inventory instance's own extra data back onto the call; this is
		// that same fix.
		//
		// It is an ENTRY hook, and the only patch in this plugin that copies
		// bytes, so it is also the only one that can corrupt code: the copied
		// prologue has to end on an instruction boundary and must not carry an
		// operand that only means anything at its original address. The prologue
		// is decoded before anything is written, and every unexpected answer
		// refuses the hook and leaves the engine exactly as it was.
		using EquipObject_t = void (*)(
			RE::ActorEquipManager*,  // this
			RE::Actor*,
			RE::TESBoundObject*,
			RE::ExtraDataList*,
			std::uint32_t,
			const RE::BGSEquipSlot*,
			bool,
			bool,
			bool,
			bool);

		EquipObject_t EquipObject_orig{ nullptr };

		struct EquipItemLookup
		{
			RE::ExtraDataList* extraData;
			bool               matched;
			bool               worn;
		};

		// The engine's own visitor, ported: the first inventory entry of the
		// matching form, its first extra list as the instance, and "already
		// worn" if any of that entry's extra lists carries the worn flag.
		void FindEquipItemExtraData(RE::Actor* a_actor, RE::TESBoundObject* a_form, EquipItemLookup* a_out)
		{
			auto* changes = a_actor->GetInventoryChanges(true);
			if (!changes || !changes->entryList) {
				return;
			}

			for (auto* entry : *changes->entryList) {
				if (!entry || entry->object != a_form) {
					continue;
				}

				a_out->matched = true;

				if (!entry->extraLists) {
					return;
				}

				bool first = true;
				for (auto* xList : *entry->extraLists) {
					if (first) {
						a_out->extraData = xList;
						first = false;
					}
					if (xList && (xList->HasType<RE::ExtraWorn>() || xList->HasType<RE::ExtraWornLeft>())) {
						a_out->worn = true;
						break;
					}
				}
				return;
			}
		}

		// Split out so the walk can sit under __try, which MSVC refuses beside
		// objects with destructors.
		[[nodiscard]] bool TryFindEquipItemExtraData(RE::Actor* a_actor, RE::TESBoundObject* a_form, EquipItemLookup* a_out)
		{
			__try {
				FindEquipItemExtraData(a_actor, a_form, a_out);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void EquipObject_Hook(
			RE::ActorEquipManager*  a_this,
			RE::Actor*              a_actor,
			RE::TESBoundObject*     a_object,
			RE::ExtraDataList*      a_extraData,
			std::uint32_t           a_count,
			const RE::BGSEquipSlot* a_slot,
			bool                    a_queueEquip,
			bool                    a_forceEquip,
			bool                    a_playSounds,
			bool                    a_applyNow)
		{
			// Everything below is additive: unless a form is being equipped with
			// no extra data and the inventory holds an instance of it that is not
			// the one already worn, the arguments go through untouched.
			if (!a_extraData && a_actor && a_object && a_count > 0) {
				auto* process = a_actor->GetActorRuntimeData().currentProcess;
				if (process && a_object != process->equippedObjects[0] && a_object != process->equippedObjects[1]) {
					EquipItemLookup found{};
					if (TryFindEquipItemExtraData(a_actor, a_object, &found)) {
						if (found.matched && !found.worn) {
							SKSE::log::debug("script-equip fix: equipping {:08X} on actor {:08X} with its inventory instance's extra data ({})",
								a_object->GetFormID(), a_actor->GetFormID(), found.extraData ? "present" : "none");
							a_extraData = found.extraData;
						}
					} else {
						SKSE::log::error("script-equip lookup faulted for {:08X} on actor {:08X}; equipping unchanged",
							a_object->GetFormID(), a_actor->GetFormID());
					}
				}
			}

			EquipObject_orig(
				a_this,
				a_actor,
				a_object,
				a_extraData,
				a_count,
				a_slot,
				a_queueEquip,
				a_forceEquip,
				a_playSounds,
				a_applyNow);
		}

		// The branch goes in at the first instruction boundary at or past this
		// offset, never at the entry itself: this function's prologue branches
		// inside its first few bytes, and a copied relative branch would jump
		// into nowhere. The offset is the original plugin's own site, past the
		// prologue, and the walk below is what makes landing there safe rather
		// than assumed.
		constexpr std::size_t kPatchOffset = 0x9;
		constexpr std::size_t kPatchSize = 6;

		struct EntryPatchPlan
		{
			std::uintptr_t site;
			std::size_t    copySize;
		};

		// A relative branch keeps its displacement in the immediate field, and
		// the width follows the operand size.
		[[nodiscard]] std::int64_t RelativeDisplacement(const hde64s& a_hs)
		{
			if ((a_hs.flags & F_IMM8) != 0) {
				return static_cast<std::int8_t>(a_hs.imm.imm8);
			}
			if ((a_hs.flags & F_IMM16) != 0) {
				return static_cast<std::int16_t>(a_hs.imm.imm16);
			}
			if ((a_hs.flags & F_IMM32) != 0) {
				return static_cast<std::int32_t>(a_hs.imm.imm32);
			}
			return 0;
		}

		// Where the branch goes, and how many whole instructions have to move
		// with it. Refuses anything that would not survive being copied: an
		// operand measured from its own address, or a branch that would land in
		// the bytes being replaced.
		[[nodiscard]] bool PlanEntryPatch(std::uintptr_t a_entry, EntryPatchPlan* a_out)
		{
			// The bytes before the site keep running where they are, but a
			// branch among them must not land inside what the branch replaces.
			std::vector<std::uintptr_t> earlyTargets;
			std::size_t                 at = 0;
			while (at < kPatchOffset) {
				hde64s     hs{};
				const auto len = hde64_disasm(reinterpret_cast<const void*>(a_entry + at), &hs);
				if (len == 0 || (hs.flags & F_ERROR) != 0) {
					SKSE::log::error("script-equip fix: undecodable instruction at {:X} (+{:#x}); not hooking", a_entry + at, at);
					return false;
				}
				if ((hs.flags & F_RELATIVE) != 0) {
					earlyTargets.push_back(a_entry + at + len + static_cast<std::uintptr_t>(RelativeDisplacement(hs)));
				}
				at += len;
			}

			// The walk stopped on a boundary, so the site is one by
			// construction -- which is the whole point of not using the entry.
			a_out->site = a_entry + at;

			std::size_t copied = 0;
			while (copied < kPatchSize) {
				hde64s     hs{};
				const auto len = hde64_disasm(reinterpret_cast<const void*>(a_out->site + copied), &hs);
				if (len == 0 || (hs.flags & F_ERROR) != 0) {
					SKSE::log::error("script-equip fix: undecodable instruction at {:X} (+{:#x}); not hooking", a_out->site + copied, copied);
					return false;
				}
				if ((hs.flags & F_RELATIVE) != 0) {
					SKSE::log::error("script-equip fix: relative branch at {:X} (+{:#x}); not hooking", a_out->site + copied, copied);
					return false;
				}
				if ((hs.flags & F_MODRM) != 0 && hs.modrm_mod == 0 && hs.modrm_rm == 5) {
					SKSE::log::error("script-equip fix: rip-relative operand at {:X} (+{:#x}); not hooking", a_out->site + copied, copied);
					return false;
				}
				copied += len;
			}
			a_out->copySize = copied;

			const auto end = a_out->site + copied;
			for (const auto target : earlyTargets) {
				if (target >= a_out->site && target < end) {
					SKSE::log::error("script-equip fix: a branch before the site lands inside the copied bytes; not hooking");
					return false;
				}
			}

			return true;
		}

		// An entry patch is a six-byte `jmp [rip + disp32]` through a slot in the
		// trampoline, never a five-byte rel32 branch: the slot is allocated
		// within reach of the module by construction, where a direct branch to
		// this DLL is not guaranteed to be.
		[[nodiscard]] bool InstallScriptEquipFix()
		{
			static REL::Relocation<std::uintptr_t> entry{ REL::RelocationID(37938, 38894) };
			const auto                             addr = entry.address();

			const auto* bytes = reinterpret_cast<const std::uint8_t*>(addr);
			if (bytes[0] == 0xE9 || bytes[0] == 0xEB || bytes[0] == 0xCC ||
				(bytes[0] == 0xFF && bytes[1] == 0x25)) {
				SKSE::log::error("script-equip fix: the entry at {:X} is already patched or is a thunk ({:02X} {:02X}); not hooking",
					addr, bytes[0], bytes[1]);
				return false;
			}

			EntryPatchPlan plan{};
			if (!PlanEntryPatch(addr, &plan)) {
				return false;
			}

			const auto* site = reinterpret_cast<const std::uint8_t*>(plan.site);
			if (site[0] == 0xE9 || site[0] == 0xEB || site[0] == 0xCC ||
				(site[0] == 0xFF && site[1] == 0x25)) {
				SKSE::log::error("script-equip fix: the site at {:X} is already patched or is a thunk ({:02X} {:02X}); not hooking",
					plan.site, site[0], site[1]);
				return false;
			}

			constexpr std::size_t kJmpBackSize = 14;  // FF 25 00000000 + qword target
			auto&                 trampoline = SKSE::GetTrampoline();
			if (trampoline.free_size() < plan.copySize + kJmpBackSize + sizeof(std::uintptr_t)) {
				SKSE::log::error("script-equip fix: no room in the trampoline; not hooking");
				return false;
			}

			try {
				// The copy runs the instructions the branch overwrites, then
				// rejoins the function just past them.
				auto* copy = static_cast<std::uint8_t*>(trampoline.allocate(plan.copySize + kJmpBackSize));
				std::memcpy(copy, reinterpret_cast<const void*>(plan.site), plan.copySize);
				auto* jmp = copy + plan.copySize;
				jmp[0] = 0xFF;
				jmp[1] = 0x25;
				jmp[2] = 0;
				jmp[3] = 0;
				jmp[4] = 0;
				jmp[5] = 0;
				const auto resume = plan.site + plan.copySize;
				std::memcpy(jmp + 6, &resume, sizeof(resume));

				auto*      slot = trampoline.allocate<std::uintptr_t>();
				const auto target = reinterpret_cast<std::uintptr_t>(&EquipObject_Hook);
				std::memcpy(slot, &target, sizeof(target));

				const auto disp = reinterpret_cast<const std::uint8_t*>(slot) - reinterpret_cast<const std::uint8_t*>(plan.site + kPatchSize);
				if (disp < (std::numeric_limits<std::int32_t>::min)() || disp > (std::numeric_limits<std::int32_t>::max)()) {
					SKSE::log::error("script-equip fix: the trampoline slot is out of range of the site; not hooking");
					return false;
				}

				std::uint8_t patch[kPatchSize];
				patch[0] = 0xFF;
				patch[1] = 0x25;
				const auto disp32 = static_cast<std::int32_t>(disp);
				std::memcpy(patch + 2, &disp32, sizeof(disp32));

				// Last gate: the bytes about to be overwritten must still be the
				// ones just decoded and copied, or something else patched this
				// site between the read and the write.
				if (!REL::safe_write(plan.site, patch, kPatchSize, copy, kPatchSize)) {
					SKSE::log::error("script-equip fix: the site at {:X} changed under us; not hooking", plan.site);
					return false;
				}

				EquipObject_orig = reinterpret_cast<EquipObject_t>(copy);
			} catch (const std::exception& e) {
				SKSE::log::error("script-equip fix: hook failed: {}", e.what());
				return false;
			} catch (...) {
				SKSE::log::error("script-equip fix: hook failed (unknown exception)");
				return false;
			}

			SKSE::log::info("script-equip fix: branch installed at {:X} (entry +{:#x}, {} byte(s) copied)",
				plan.site, plan.site - addr, plan.copySize);
			return true;
		}

		// a_onlyForm: when the caller knows which item was just equipped
		// (TESEquipEvent::baseObject) only that item is considered. Walking the
		// whole inventory on every equip is what made switching weapons and
		// armour stutter; the original plugin only ever looks at the equipped
		// form. 0 means "no filter" and is what the load-time path passes,
		// since that one has to restore every worn item.
		void ProcessActor(RE::Actor* a_actor, RE::FormID a_onlyForm = 0)
		{
			if (!IsValidActor(a_actor)) {
				return;
			}

			// a_noInit = true: never initialize the inventory from here.
			auto* changes = a_actor->GetInventoryChanges(true);
			if (!changes || !changes->entryList) {
				return;
			}

			// Collect first. UpdateArmorAbility can mutate the inventory, so we must
			// NOT be iterating the entry list while calling it.
			struct Candidate
			{
				RE::TESForm*         form;
				RE::ExtraDataList*   worn;
				RE::EnchantmentItem* enchantment;
			};
			std::vector<Candidate> candidates;

			SKSE::log::debug("re-check actor {:08X} filter {:08X}", a_actor->GetFormID(), a_onlyForm);

			for (auto* entry : *changes->entryList) {
				if (!entry || !entry->object) {
					continue;
				}
				if (!entry->object->As<RE::TESObjectARMO>()) {
					continue;
				}
				if (a_onlyForm && entry->object->GetFormID() != a_onlyForm) {
					continue;
				}
				if (!entry->IsWorn()) {
					continue;
				}

				RE::ExtraDataList* worn = nullptr;
				if (entry->extraLists) {
					for (auto* xList : *entry->extraLists) {
						if (xList && (xList->HasType<RE::ExtraWorn>() || xList->HasType<RE::ExtraWornLeft>())) {
							worn = xList;
							break;
						}
					}
				}

				auto* enchantment = GetWornEnchantment(worn);
				SKSE::log::debug("  worn armour {:08X} wornList={} instanceEnch={:08X} casting={}",
					entry->object->GetFormID(), worn != nullptr,
					enchantment ? enchantment->GetFormID() : 0,
					enchantment ? static_cast<int>(enchantment->data.castingType) : -1);
				if (!enchantment) {
					continue;
				}

				if (!IsValidArmorEnchantment(enchantment)) {
					SKSE::log::debug(
						"skipping non-constant enchantment {:08X} on worn armour {:08X} (actor {:08X})",
						enchantment->GetFormID(),
						entry->object->GetFormID(),
						a_actor->GetFormID());
					continue;
				}

				candidates.push_back({ entry->object, worn, enchantment });
			}

			for (auto& c : candidates) {
				const auto check = TryHasItemAbility(a_actor, c.form, c.enchantment);
				SKSE::log::debug("  ability check {:08X}/{:08X} -> {}", c.form->GetFormID(),
					c.enchantment->GetFormID(),
					check == AbilityCheck::kPresent ? "present" : check == AbilityCheck::kMissing ? "missing" : "unknown");
				if (check != AbilityCheck::kMissing) {
					continue;
				}

				SKSE::log::info(
					"  re-applying enchantment ability for {:08X} on actor {:08X}",
					c.form->GetFormID(),
					a_actor->GetFormID());

				if (!TryUpdateArmorAbility(a_actor, c.form, c.worn)) {
					SKSE::log::error("UpdateArmorAbility faulted; skipped");
				}
			}
		}

		// Queue an actor for a re-check. The handle only goes into a set here --
		// no form-table lookup, no per-actor task -- and exactly ONE drain task
		// is queued for the whole batch. On a save load the engine fires these
		// events for thousands of references, and one queued task each is what
		// froze the main thread for seconds.
		void ScheduleActorCheck(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return;
			}

			auto* actor = a_ref->As<RE::Actor>();
			// Gate here, while the pointer is guaranteed live (it comes straight
			// from the event): never queue a check for an actor that is already
			// deleted or dead.
			if (!IsValidActor(actor)) {
				return;
			}

			RE::ObjectRefHandle handle(actor);
			if (!handle) {
				return;
			}

			SKSE::log::debug("queued re-check for {:08X}", actor->GetFormID());

			{
				std::scoped_lock lock(s_queueLock);
				s_pending.insert(handle);
				if (s_drainQueued) {
					return;
				}
				s_drainQueued = true;
			}

			auto* taskInterface = SKSE::GetTaskInterface();
			if (!taskInterface) {
				std::scoped_lock lock(s_queueLock);
				s_drainQueued = false;
				return;
			}

			taskInterface->AddTask([]() {
				const auto started = std::chrono::steady_clock::now();

				std::unordered_set<
					RE::ObjectRefHandle,
					RE::BSCRC32<RE::ObjectRefHandle>>
					batch;

				{
					std::scoped_lock lock(s_queueLock);
					batch.swap(s_pending);
					s_drainQueued = false;
				}

				// The handle resolves to a live reference-counted pointer, so a
				// stale actor simply resolves to nothing here.
				std::size_t processed = 0;
				for (const auto& handle : batch) {
					auto ref = handle.get();
					if (!ref) {
						continue;
					}
					if (auto* loaded = ref->As<RE::Actor>(); IsValidActor(loaded)) {
						ProcessActor(loaded);
						++processed;
					}
				}

				// Whether the load-time freeze is ours is a measurement, not a
				// guess: a long span here (or a huge batch) is us, a short one
				// means the time goes somewhere else. One line per drain is
				// cheap, so this stays on.
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
									std::chrono::steady_clock::now() - started)
									.count();
				SKSE::log::info(
					"drain: {} queued, {} processed, {} ms",
					batch.size(),
					processed,
					ms);
			});
		}

		void LoadSettings()
		{
			CSimpleIniA ini;
			ini.SetUnicode();

			const auto rc = ini.LoadFile(kIniPath);
			if (rc < 0) {
				SKSE::log::info("no ini found at {}, using defaults", kIniPath);
			}

			s_onEquip = ini.GetBoolValue("EEF", "OnEquip", true);
			s_onActorLoad = ini.GetBoolValue("EEF", "OnActorLoad", true);
			s_recalcWeightOnLoad = ini.GetBoolValue("EEF", "RecalcPlayerInventoryWeightOnLoad", false);
			s_redirectDispel = ini.GetBoolValue("EEF", "RedirectDispelWornItemEnchantsVisitor", true);
			s_scriptEquipEventFix = ini.GetBoolValue("EEF", "ScriptEquipEventFix", false);

			// "debug" shows every step of the redirect and the re-check.
			if (const auto* level = ini.GetValue("EEF", "LogLevel", nullptr)) {
				spdlog::set_level(spdlog::level::from_str(level));
				spdlog::flush_on(spdlog::level::from_str(level));
			}

			SKSE::log::info(
				"settings: OnEquip={} OnActorLoad={} RecalcWeight={} RedirectDispel={} ScriptEquipEventFix={}",
				s_onEquip,
				s_onActorLoad,
				s_recalcWeightOnLoad,
				s_redirectDispel,
				s_scriptEquipEventFix);
		}
	}

	void ClearPendingChecks()
	{
		std::scoped_lock lock(s_queueLock);
		s_pending.clear();
		s_drainQueued = false;
	}

	void RecalcPlayerWeight()
	{
		if (!s_recalcWeightOnLoad) {
			return;
		}

		auto* taskInterface = SKSE::GetTaskInterface();
		if (!taskInterface) {
			return;
		}

		taskInterface->AddTask([]() {
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			if (auto* changes = player->GetInventoryChanges()) {
				changes->totalWeight = -1.0F;
			}
		});
	}

	RE::BSEventNotifyControl EquipEventHandler::ProcessEvent(
		const RE::TESEquipEvent*               a_event,
		RE::BSTEventSource<RE::TESEquipEvent>*)
	{
		// No form-table lookup here on purpose: this event fires while the
		// engine's form-table lock is in play, and looking the base object up
		// from here crashes. ProcessActor already walks the worn list and only
		// touches armour, so the lookup bought us nothing but a crash.
		if (s_onEquip && a_event && a_event->equipped && a_event->actor) {
			if (auto* actor = a_event->actor->As<RE::Actor>(); IsValidActor(actor)) {
				// baseObject is a FormID read straight off the event -- no
				// form-table lookup, which is what used to crash in this
				// handler. It tells ProcessActor to consider only the item that
				// was just equipped instead of rescanning the whole inventory.
				ProcessActor(actor, a_event->baseObject);
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl LoadEventHandler::ProcessEvent(
		const RE::TESObjectLoadedEvent*               a_event,
		RE::BSTEventSource<RE::TESObjectLoadedEvent>*)
	{
		// Deliberately cheap: resolve-and-queue only, no work here. The
		// single shared drain task (see ScheduleActorCheck) is what bounds
		// the load-time burst -- one task no matter how many references load.
		if (a_event && a_event->loaded) {
			if (auto* ref = LookupFormSafe<RE::TESObjectREFR>(a_event->formID)) {
				ScheduleActorCheck(ref);
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl LoadEventHandler::ProcessEvent(
		const RE::TESInitScriptEvent*               a_event,
		RE::BSTEventSource<RE::TESInitScriptEvent>*)
	{
		if (a_event && a_event->objectInitialized) {
			ScheduleActorCheck(a_event->objectInitialized.get());
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	EquipEventHandler* EquipEventHandler::GetSingleton()
	{
		static EquipEventHandler singleton;
		return &singleton;
	}

	LoadEventHandler* LoadEventHandler::GetSingleton()
	{
		static LoadEventHandler singleton;
		return &singleton;
	}

	bool Initialize()
	{
		LoadSettings();

		const auto holder = RE::ScriptEventSourceHolder::GetSingleton();
		if (!holder) {
			SKSE::log::error("ScriptEventSourceHolder unavailable");
			return false;
		}

		if (s_onEquip) {
			holder->AddEventSink<RE::TESEquipEvent>(EquipEventHandler::GetSingleton());
		}
		if (s_onActorLoad) {
			holder->AddEventSink<RE::TESObjectLoadedEvent>(LoadEventHandler::GetSingleton());
			holder->AddEventSink<RE::TESInitScriptEvent>(LoadEventHandler::GetSingleton());
		}
		if (s_redirectDispel) {
			// Without the block, the armour trade's re-equip would stack a copy of
			// what the redirect kept, so the redirect only goes in after it.
			const bool blocked = InstallUpdateArmorAbilityHook();
			if (!blocked) {
				SKSE::log::warn("UpdateArmorAbility hook unavailable; the dispel redirect is not installed either, and worn enchantments will drop on an inventory change");
			} else if (!InstallDispelRedirect()) {
				SKSE::log::warn("dispel redirect unavailable; worn enchantments will still drop on an inventory change");
			}
		}

		if (s_scriptEquipEventFix) {
			if (!InstallScriptEquipFix()) {
				SKSE::log::warn("script-equip fix unavailable; a scripted equip will not raise OnEquipped for the item");
			}
		}

		SKSE::log::info(
			"registered sinks (OnEquip={} OnActorLoad={} RedirectDispel={} ScriptEquipEventFix={})",
			s_onEquip,
			s_onActorLoad,
			s_redirectDispel,
			s_scriptEquipEventFix);

		return true;
	}
}
