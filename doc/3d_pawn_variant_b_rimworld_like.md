# 3D Pawn Specification — Variant B: “RimWorld Pawn but 3D”

## Goal

Create a minimal stylized modular 3D pawn for a colony sim / RTS.

The target visual idea is:

> **RimWorld-like pawn readability, but as a simple 3D character.**

The model should be intentionally simple, highly readable from a top-down / isometric camera, cheap to render in large numbers, and easy to recolor and equip.

This is **not** a realistic human character.

---

## Visual Style

The pawn should have:

- slightly oversized head;
- compact rounded torso;
- thin/simple arms;
- no visible fingers;
- no detailed facial anatomy;
- no realistic musculature;
- simple chunky silhouette;
- low-poly stylization;
- proportions optimized for readability from a distance.

The result should feel like a 3D interpretation of a colony-sim pawn rather than a conventional humanoid game character.

---

## Core Hierarchy

```text
PawnRoot
├── Body
├── Head
├── Arm_L
├── Arm_R
├── Hair
├── Beard
├── Headwear
├── Clothing
├── BackItem
└── Weapon
```

Each major part should be a separate rigid object.

No skeletal rig is required.

---

## Base Body

### Body

Requirements:

- simplified rounded torso;
- slightly wider toward the upper body;
- no detailed chest or abdominal anatomy;
- no visible legs required;
- silhouette should remain readable from top-down;
- body should work as a base under clothing shells.

Suggested form:

```text
rounded capsule
or
rounded tapered block
```

The body should not look like a realistic human torso.

---

## Head

Requirements:

- slightly oversized relative to the torso;
- simple round / oval shape;
- minimal or no facial geometry;
- suitable for attaching:
  - hair,
  - beard,
  - helmets,
  - hats,
  - race-specific head elements.

The head should remain clearly visible from the gameplay camera.

---

## Arms

Use two independent rigid arm objects:

```text
Arm_L
Arm_R
```

Requirements:

- simple capsule / tapered cylinder;
- no fingers;
- no detailed elbows;
- pivot positioned at the shoulder;
- suitable for procedural swinging and attack motions;
- capable of holding weapon objects.

The arms do not need deformation.

---

## No Legs Requirement

Default pawn should not require visible legs.

Movement readability should come from:

- body bob;
- body tilt;
- arm swing;
- slight head motion.

If later required, very small stylized feet may be added as optional modules, but the base pawn should function without them.

---

## Mesh Budget

Recommended target:

```text
LOD0: 250–600 triangles total
LOD1: 120–250 triangles
LOD2: 40–120 triangles
```

LOD0 should include the base pawn only.

Hair, beard, clothing, weapons and equipment may add additional triangles, but should remain aggressively low-poly.

---

## Modularity

The pawn must be designed as a modular character system.

### Replaceable modules

```text
Head
Hair
Beard
Headwear
Clothing
BackItem
Weapon
```

Optional future modules:

```text
Ears
Horns
Tusks
Snout
Tail
Wing elements
Race-specific head shapes
```

All modules should align to stable attachment points.

---

## Pivot Requirements

### PawnRoot

Pivot:

```text
center of character footprint at ground level
```

Used for:

- world positioning;
- rotation;
- movement;
- global bob animation.

### Body

Pivot:

```text
bottom-center or lower torso center
```

### Head

Pivot:

```text
neck attachment point
```

### Arms

Pivot:

```text
shoulder joint
```

### Weapon

Pivot should support easy attachment to a hand / arm transform.

### BackItem

Pivot should align consistently to the rear center of the torso.

---

## Animation Model

No skeletal animation.

All animation should be transform-based.

### Idle

Use subtle procedural movement:

```text
small body sway
small vertical bob
small head sway
minor asymmetric arm movement
```

Example:

```cpp
bodyOffsetY = sin(time * idleFreq + seed) * idleAmplitude;
headRotZ    = sin(time * headFreq + seed * 1.7) * headSway;
armLRotZ    = sin(time * armFreq + seed) * armIdle;
armRRotZ    = sin(time * armFreq + seed + 2.1) * armIdle;
```

---

## Movement Animation

Movement should remain extremely simple.

Use:

```text
body bob
slight forward tilt
alternating arm swing
optional head bob
```

Example:

```cpp
float phase = time * moveFrequency + instanceSeed;

body.yOffset = sin(phase * 2.0) * bobAmount;
body.rotationX = forwardLean;

armL.rotation = sin(phase) * armSwing;
armR.rotation = sin(phase + PI) * armSwing;

head.yOffset = sin(phase * 2.0 + 0.4) * headBob;
```

No footstep animation is required.

---

## Attack / Work Animation

Use rigid transforms only.

Example sequence:

```text
1. arm pulls back
2. torso leans slightly
3. arm swings forward
4. torso returns
```

Suitable for:

```text
sword attack
axe chop
hammering
mining
woodcutting
farming
```

Different actions should reuse the same simple transform framework.

---

## Per-Instance Variation

Each pawn should have a deterministic random seed.

Use it to vary:

```text
idle phase
bob phase
arm swing amplitude
head sway
body scale
head scale
shoulder width
animation frequency
```

This prevents large groups from moving synchronously.

---

## Recommended Parameters

```text
body_height
body_width
body_depth
head_scale
head_height
arm_length
arm_thickness
shoulder_width
body_roundness
```

Possible visual variation:

```text
thin
average
broad
short
tall
large_head
small_head
```

These parameters should modify the base shape without requiring unique characters.

---

## Materials

Prefer a very small number of material slots.

Suggested:

```text
Skin
Clothing
Hair
Equipment
```

If possible, use shared materials across many pawns.

Use per-instance colors where supported.

Example character color parameters:

```text
skinColor
hairColor
clothingPrimary
clothingSecondary
metalColor
```

---

## UV Requirements

Use simple atlas-friendly UVs.

Goals:

- easy recoloring;
- minimal texture memory;
- reusable materials;
- compatibility with material arrays / texture atlases;
- no requirement for unique high-resolution textures per pawn.

Avoid UV complexity that provides no visible benefit from the gameplay camera.

---

## Equipment

Weapons should remain independent rigid meshes.

Examples:

```text
sword
axe
spear
bow
club
shield
tool
```

Recommended attachment structure:

```text
Arm_R
└── Weapon

Arm_L
└── Shield
```

Weapons should not require changes to the base pawn mesh.

---

## Clothing

Clothing should be separate low-poly shells placed over the body.

Examples:

```text
simple tunic
robe
leather vest
armor shell
royal tunic
priest outfit
```

Do not simulate cloth.

Long clothing may use one rigid mesh or a small number of rigid pieces.

---

## Hair and Beard

Hair and beard should be simple modular meshes.

Requirements:

- readable from above;
- strong silhouette;
- very low polygon count;
- no strand simulation;
- no hair physics.

Examples:

```text
short hair
long back hair
ponytail
braid
short beard
large beard
forked beard
```

---

## Race Compatibility

The system should support future races without replacing the whole pawn architecture.

Examples:

```text
Human
Elf
Dwarf
Orc
Catfolk
Birdfolk
Lizardfolk
Walrus
```

Race differences should preferably be implemented through:

```text
head replacement
body proportions
ears
snout
horns
tusks
tail
skin material
```

The core hierarchy should remain compatible.

---

## Rendering Considerations

This pawn system is intended for large-scale RTS / colony-sim scenes.

Priorities:

1. low triangle count;
2. shared materials;
3. GPU instancing where possible;
4. deterministic procedural animation;
5. aggressive LOD;
6. simple silhouettes;
7. minimal CPU animation work.

Avoid:

```text
SkinnedMeshRenderer-style deformation
complex skeletons
per-character animation graphs
unique textures per pawn
high-frequency facial details
expensive cloth/hair simulation
```

---

## Suggested LOD Behaviour

### LOD0

Visible at close gameplay distance.

Includes:

```text
full body
head
arms
hair
beard
clothing
equipment
```

### LOD1

Simplify:

```text
reduced polygon meshes
simplified hair
simplified equipment
less visible body curvature
```

### LOD2

Use:

```text
very simple silhouette mesh
or
impostor / billboard
```

At long RTS distance, individual anatomical detail is unnecessary.

---

## Deliverables

The agent should produce:

```text
BasePawn.blend
BasePawn.glb
BasePawn.fbx
```

And preferably:

```text
LOD0
LOD1
LOD2
```

Base content:

```text
1 base pawn
1 simple clothing variant
1 simple hair variant
1 beard variant
1 helmet variant
1 placeholder weapon
```

---

## Validation Checklist

The result is acceptable if:

- pawn is immediately readable from top-down;
- silhouette remains clear at RTS camera distance;
- no skeletal rig is required;
- arms can rotate independently around shoulder pivots;
- head can rotate independently;
- clothing can be swapped;
- hair and beard can be swapped;
- weapon can be attached to an arm;
- body bob + arm swing is enough to communicate movement;
- base pawn remains under the target triangle budget;
- model can be efficiently instanced;
- design can be reused for multiple races and professions.

---

## Final Design Principle

The character should be treated as:

```text
a modular animated game token
```

rather than:

```text
a miniature realistic human character
```

The priority order is:

```text
readability
> modularity
> performance
> animation simplicity
> anatomical realism
```
