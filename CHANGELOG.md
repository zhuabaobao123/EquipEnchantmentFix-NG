# Changelog

All notable changes to the NG port. 1.3.7 - 1.3.23 have been released;
1.3.19 was an internal build that was folded into 1.3.20.

## 1.3.23

**Added: `ScriptEquipEventFix` (off by default), for mods that equip by script.**
Papyrus `Actor.EquipItem` reaches the engine with no extra data, so the engine
equips a bare instance and the item's `OnEquipped` never fires. With this on, the
inventory instance's own extra data is put back onto that call. Torch Mechanics
Fixed names the option as a requirement.

- Off by default, as in the original. Nothing changes unless you turn it on.
- The one patch here that copies engine bytes: the bytes are decoded first and the
  branch goes in at an instruction boundary past the prologue, never at the entry.
  Anything it cannot prove refuses the hook and leaves the engine untouched.

## 1.3.22

**Fixed: trading with a follower stripped the enchantments off their worn
armour.** Giving or taking any item in the trade menu, even a potion, removed
every enchantment on the armour they were wearing. They came back only after a
reload, or after closing the menu if armour had changed hands. Worn enchantments
now stay put through a trade, and the follower's stats are right while the menu
is still open.

- Works on any game version the Address Library covers: the plugin locates the
  engine calls it patches at launch instead of carrying per-version offsets, and
  leaves the engine alone if it cannot find them.
- Removed the "re-check after an effect is removed" path from 1.3.9; it never
  triggered for worn enchantments.
- New `LogLevel` ini setting: `info` by default, `debug` for troubleshooting.

## 1.3.21

**The engine's duplicate apply is now blocked before it happens** - the same
mechanism the original plugin used.

- Hooks the engine's call to `Actor::UpdateArmorAbility` and skips it when the
  actor already carries that item's enchantment, matched on the same
  `(source, spell)` pair the repair path uses. 1.3.20 could only restore an
  enchantment after a bad dispel; this stops the duplicate apply at source, so
  nothing blinks.
- `RedirectDispelWornItemEnchantsVisitor` now defaults to **true**, matching the
  original plugin. The post-hoc recheck stays as the fallback if the hook cannot
  be installed.
- The hook patches the call site only after verifying the byte there really is a
  `call` (`E8`/`E9`). If a future runtime moves it, the plugin logs and falls back
  to the recheck rather than patching blind.
- Hooking the call site rather than the function keeps our own repair calls
  working, since they reach the function directly.
- Verified in game on SE 1.5.97: 42 unequip/equip cycles of an enchanted item
  leave exactly one effect; a real unequip still removes it and re-equipping
  restores it; load drain 0 ms; no errors in any log.

- **License corrected to GPL-3.0-or-later.** This plugin statically links
  CommonLibSSE-NG, which is GPL-3.0-or-later and states that a linking plugin
  forms a combined work with it and must be GPL as well. The MIT text it
  inherited from the original plugin is kept for attribution only (see
  LICENSES/). Thanks to ChrysopoeiaAlchemy for pointing this out.

## 1.3.20

**Fixed: enchantments could stack on repeated equips** (the duplicate-enchantment
report that came in after 1.3.18).

The "is this enchantment already on the actor" check was `Actor::HasMagicEffect`.
That call **faults on every runtime tested, SE 1.5.97 included** - so it never
worked reliably: the failure path reported "missing", every equip looked like it
needed re-applying, and effects accumulated.

It is replaced with the test the original plugin used: walk the actor's active
effects and match the `(source, spell)` pair, where `source` is the item and
`spell` is its enchantment. `MagicTarget::GetActiveEffectList` is reached through
`AsMagicTarget()`, which supplies the correct `MagicTarget` offset (0x98 on SE,
0xA0 on AE); a bare `Actor*` would read the wrong vtable.

- Equipping no longer rescans the whole inventory: the equip event re-checks only
  the item that was actually equipped. This removes the stutter when switching
  weapons or armour.
- Removed the re-apply cooldown, the fault counter and the tri-state ability
  result - all of them existed only to work around the broken query above.

Verified in game on SE 1.5.97: 25 unequip/equip cycles of an enchanted item leave
exactly one effect each time; no faults; no spurious re-applies.

**On the loading freeze:** the load-time actor re-check is measured at **0 ms**
for ~450 actors. It is not the source of the multi-second load freeze reported
against 1.3.18.

## 1.3.18
- Removed the post-load actor sweep (it could apply the same item twice in one
  frame) and added a short re-apply cooldown. **Superseded by 1.3.20** - the
  cooldown is gone and the underlying check is now exact.

## 1.3.17
- Restored `TESObjectLoadedEvent` and batched the load-time re-checks into a
  single task, so streaming references no longer queue thousands of tasks.

## 1.3.16
- Replaced the per-reference load-time task with an actor-list sweep.

## 1.3.15
- Removed the temporary diagnostic logging.

## 1.3.14
- Swapped the active-effect walk for `Actor::HasMagicEffect` to stop the crash in
  `GetActiveEffectList` / `VisitEffects`. **Reverted in 1.3.20** - that call turned
  out to fault as well, and silently disabled the check.

## 1.3.13
- Used `MagicTarget::VisitEffects`.

## 1.3.11
- Added SEH (structured exception handling) around the fault-prone engine calls.

## 1.3.10
- Removed the form-table lookup from the equip handler (it crashed while the
  engine's form-table lock was held).

## 1.3.9
- Skip enchantments that are not constant-effect: a weapon enchantment moved onto
  armour by a "no enchantment restriction" mod drives the engine down a branch it
  does not expect and crashes.
- `RedirectDispelWornItemEnchantsVisitor` defaults to off.

## 1.3.8
- Fixed a crash in the deferred task.

## 1.3.7
- First CommonLibSSE-NG build.
