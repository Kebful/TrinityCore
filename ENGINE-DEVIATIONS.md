# ENGINE-DEVIATIONS — the FFXIV damage/heal spine

Every deviation from stock WoW combat that this slice makes, the stage it replaces, why it exists, and
how to tell a deliberate change from a bug. Code lives in
`src/server/game/FFXIV/Combat/FFXIVSpellStages.{h,cpp}` and `FFXIVActionDamage.{h,cpp}`; the call
sites are the few `@ffxiv-begin` / `@ffxiv-end` blocks marked in `Entities/Unit/Unit.cpp`,
`Spells/Spell.cpp` and `Spells/SpellEffects.cpp`.

Selection rule: a spell is an **FFXIV action spell** when it has an `ffxiv_action_spell` row and the
Action that row names has at least one `ffxiv_action_potency` row. There is no id list; an empty
table makes every rule below inert and restores stock WoW exactly.

## 1. The numbers are final before the engine sees them

| # | Stage replaced | Deviation | Why |
|---|---|---|---|
| D1 | `Unit::SpellDamageBonusDone` | identity for an FFXIV action spell | the value already contains f(ATK), f(DET), f(TNC), f(WD), trait, crit/DH, variance and level correction (FFXIVCombatMath). Spell power / school / flat-versus / done-% would scale it twice. |
| D2 | `Unit::SpellDamageBonusTaken` | identity | same; taken-% is not an FFXIV stage. |
| D3 | `Unit::SpellHealingBonusDone` / `SpellHealingBonusTaken` | identity | the cure chain already contains f(HMP). |
| D4 | `Unit::CalcArmorReducedDamage` | input damage returned unchanged (spell rows only) | FFXIV has no WoW armour mitigation; physical potency is already final. Melee auto-attacks (no `SpellInfo`) keep the stage. |
| D5 | `Unit::CalcSpellResistedDamage` | 0 resisted | no partial resist for an action. Absorbs (shields) are **not** removed: `CalcAbsorbResist` still runs and the absorb fields still reach the log. |
| D6 | mitigation in `Spell::AddUnitTarget` | miss / resist / dodge / parry / block / deflect → `SPELL_MISS_NONE` | the spells carry `IGNORE_HIT_RESULT`; this is the same rule stated once in `NeutraliseFfxivAvoidance`. Evade, immunity and reflect stay: they are not rolls. |
| D7 | crit chance in `Spell::DoAllEffectOnTarget` | engine chance = 0 | FFXIVCombatMath rolls crit per target; two rolls would disagree. |
| D8 | melee/ranged crit bonus in `Unit::CalculateSpellDamageTaken`; `Unit::SpellCriticalDamageBonus`; `Unit::SpellCriticalHealingBonus` | crit multiplier identity, but `SPELL_HIT_TYPE_CRIT` is still set | the value already includes f(CRIT). The flag exists so the combat log and procs see a critical hit. |
| D9 | block / resilience inside `Unit::CalculateSpellDamageTaken` | skipped for the spell | D6/D4 equivalents on the same path (a block cannot reach a spell that never rolls one). |
| D10 | `CalculateAOEAvoidance` and the 10-target AoE damage cap in `DoEffectOnLaunchTarget` | skipped for the spell | per-target avoidance and the anti-zerg cap would shrink an FFXIV area action's final number; the client's per-target rows are not the server's. |

Deliberate vs bug: with the potency table empty, every rule above is a no-op. If an FFXIV spell still
crits/misses/mitigates like a WoW spell, the cause is data (no `ffxiv_action_spell` or no potency
row), not the guard. If a **non**-FFXIV spell loses mitigation, the predicate is wrong — check
`HasFfxivActionPotencyRowForAction`.

## 2. Crit and variance are per target, not per cast

`FfxivActionRollSeed(caster, action, target, ms)` mixes the **target's** guid. The old DUMMY hook used
a cast-wide seed, so an area action crit or not as a block and every target shared one variance roll.
Each `ComputeFfxivActionEffect*` call builds its own `FfxivSeededRollSource` from that seed. The
per-target crit is recorded on the `Spell` (`m_ffxivActionCrit`) and applied to the target's
`TargetInfo::IsCrit` in `DoDamageAndTriggers`, because the engine roll was neutralised at D7.

## 3. A converted spell deals through the real hit path

Converted effects (`EffectSchoolDMG`, `EffectWeaponDmg`, `EffectHeal`) compute the final number and
**add it to `m_damage` / `m_healing`**; they do not call `DealSpellDamage` themselves. That is the
deliberate difference from the older DUMMY hook: the normal `TargetInfo::DoDamageAndTriggers` path
then performs absorb, threat, proc flags and the combat log with a real spell event. The value is
still not scaled, because D1–D9 make every stage it passes identity.

The remaining DUMMY hook (`TryApplyFfxivActionEffect`) is unchanged for unconverted actions. It must
not run for a converted action (there is no DUMMY effect on one), so a spell cannot double-hit.

## 4. Effect handlers read our tables; the spell row is the tuning surface

| Handler | Value |
|---|---|
| `EffectSchoolDMG` / `EffectWeaponDmg` | selection (combo/positional from `ffxiv_action_potency`) → `FFXIVCombatMath` |
| `EffectHeal` | cure column → `FFXIVCombatMath` heal chain |
| `EffectApplyAura` | periodic damage → `ComputeFfxivDotTick` at apply; damage-%/max-HP/speed → `ffxiv_status_effect` |
| `EffectEnergize` | `ffxiv_action_effect` gauge delta → `AddFfxivGauge` (never WoW rage) |

Precedence: the **spell chain's literal** is the sheet's base band; the **potency table** wins only
where it carries a higher band (a row above the action's base minLevel) or a positional/combo
variant the chain has no single number for. That is why Heavy Swing's chain says 150 and a level-94
cast still deals 240:

* per spell: the damage/cure/DoT/gauge literal, and (for a self-carried DoT) its cadence and
  duration;
* table-side: per-level bands and positional/combo variants, because one DBC value cannot express
  “150 at level 1, 240 at 94” and the sheet already owns that tree.

The chain's numbers are written with `ShiftedNumberCell.set(value, "EFFECTIVE")` where the cell is
shifted by `EffectDieSides` (a plain `.set(150)` would store 150 and the engine would add the die
side again). A percentage aura stores `value - 1` for the same reason.

`EffectEnergize` for an FFXIV action never calls the vanilla power path. For a script-owned job the
livescript must not add the same grant again (`livescripts/job-resources/beastgauge.ts` now returns 0
for Infuriate and the spell's ENERGIZE effect owns the +50). That is a deliberate division: the spell
row is the grant, the script is only the cost/refusal authority.

## 5. Tooltips resolve at display time (client)

The descriptors use the evaluator's existing macros, with the **effect slot** as the only argument
(`@Value(1)`, `@TickValue(1)`, `@Duration(1)`, `@Period(1)`); no game value appears in the text.
`@Value(n)` on an FFXIV action spell resolves through the server's potency list (sub-opcode
`0x7A61`), keyed by the player's job and level, falling back to the effect's PointsBase.
`@Duration(n)` on an action reads the action spell's own duration. The client extension lives in
`SpellTooltipExtensions.cpp`; the macros themselves are unchanged. No “Potency: N” line is re-added
by the client; the number lives in the sentence.

Wire deviation: schema 2 of `0x7A61` widened each entry from `{actionId, damage}` (4 bytes) to
`{actionId, damage, cure, dot}` (8 bytes) so cure/DoT tooltips are level-aware too. A schema-1 client
rejects the body whole (intended tolerance); the server never sends schema 1.

## 6. What is intentionally NOT deviated

* Absorbs, immunities, reflects, evade: real mechanics, kept.
* Threat, proc flags and the combat log: produced by the normal spell path (that is the point of §3).
* Melee auto-attacks and every non-FFXIV spell: untouched by D1–D9 (no potency table → predicate
  false).
* The status apply path (`ApplyFfxivActionStatuses`, `AddAura`): unchanged; it already writes our DoT
  tick into the carrier aura.
