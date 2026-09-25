# Modular Pawn Equipment & Race Compatibility Specification

## 1. Goal

Create a modular 3D pawn equipment system for a stylized colony sim / RTS.

The system must support:

- multiple humanoid and non-humanoid races;
- different body proportions;
- modular clothing;
- helmets and headwear;
- weapons;
- shields;
- backpacks and back items;
- hair and beards;
- race-specific body features;
- rigid-part transform animation instead of skeletal skinning;
- large unit counts with low rendering and CPU cost.

The key requirement is that content generation must follow a strict compatibility contract.

The goal is not to make every item universally compatible with every race.

The goal is to make compatibility **predictable, explicit, and automatable**.

---

# 2. Core Design Principle

Do not treat equipment as arbitrary meshes that are manually fitted to individual characters.

Use:

```text
Race
  ↓
Body Family
  ↓
Body Profile
  ↓
Attachment Standard
  ↓
Compatible Equipment Variant
```

Example:

```text
Human
  └── Humanoid_Medium
      └── Average
          ├── Bronze_Tunic_HM_Average
          ├── Bronze_Cuirass_HM_Average
          ├── KettleHelmet_HumanoidHead
          └── Spear_Universal
```

A dwarf may use the same weapon but a different armor shell:

```text
Dwarf
  └── Humanoid_ShortBroad
      └── Broad
          ├── Bronze_Cuirass_HSB_Broad
          └── Spear_Universal
```

---

# 3. Pawn Hierarchy

Recommended runtime hierarchy:

```text
PawnRoot
├── BodyRoot
│   ├── Body
│   ├── Clothing
│   ├── ChestAttachment
│   ├── Hip_L
│   ├── Hip_R
│   └── Back_Center
│
├── HeadRoot
│   ├── Head
│   ├── Hair
│   ├── Beard
│   ├── Headwear
│   ├── HeadTop
│   ├── HeadFront
│   └── HeadBack
│
├── Arm_L
│   └── Hand_L
│
└── Arm_R
    └── Hand_R
```

Optional race-specific nodes:

```text
TailRoot
Horn_L
Horn_R
Ear_L
Ear_R
SnoutRoot
Wing_L
Wing_R
Tusks
CrestRoot
```

These nodes should only exist for races that require them.

---

# 4. Attachment Socket Standard

All attachment sockets must use consistent naming, orientation, and scale.

Recommended sockets:

```text
HeadTop
HeadFront
HeadBack
Neck
Chest
Back_Center
Hip_L
Hip_R
Shoulder_L
Shoulder_R
Hand_L
Hand_R
TailRoot
```

Optional sockets:

```text
Horn_L
Horn_R
Ear_L
Ear_R
Wing_L
Wing_R
Belt_Front
Belt_Back
Quiver
ShieldMount
```

## Coordinate conventions

All assets should use the same world convention.

Recommended:

```text
+Y = up
+Z = forward
+X = right
units = meters
```

Every equipment asset must preserve this convention.

---

# 5. Race Architecture

Each race must define a `RaceBodySpec`.

Example:

```cpp
struct RaceBodySpec
{
    RaceId race;

    BodyFamily bodyFamily;
    BodyProfile defaultProfile;

    float bodyHeight;
    float bodyWidth;
    float bodyDepth;

    float headScale;
    float shoulderWidth;
    float armLength;
    float armThickness;

    float handScale;
    float neckHeight;

    bool hasTail;
    bool hasHorns;
    bool hasSnout;
    bool hasExternalEars;
    bool hasWings;

    EquipmentCompatibility compatibility;
};
```

---

# 6. Body Families

Do not create one body type per race if several races can share the same compatibility standard.

Use broader body families.

Suggested families:

```text
Humanoid_Medium
Humanoid_Slender
Humanoid_ShortBroad
Humanoid_Large
Digitigrade_Humanoid
Avian_Humanoid
Reptilian_Humanoid
Heavy_NonHuman
```

Possible mapping:

```text
Human       -> Humanoid_Medium
Elf         -> Humanoid_Slender
Dwarf       -> Humanoid_ShortBroad
Orc         -> Humanoid_Large
Catfolk     -> Digitigrade_Humanoid
Birdfolk    -> Avian_Humanoid
Lizardfolk  -> Reptilian_Humanoid
Walrus      -> Heavy_NonHuman
```

This allows equipment to be authored for families instead of every individual race.

---

# 7. Body Profiles

Within one body family, support a small number of profiles.

Example:

```text
Slim
Average
Broad
Tall
Short
Heavy
```

Not every family needs all profiles.

Example:

```text
Humanoid_Medium:
    Slim
    Average
    Broad

Humanoid_ShortBroad:
    Average
    Broad

Humanoid_Large:
    Average
    Heavy
```

Avoid excessive combinations.

For initial production, one or two profiles per body family are enough.

---

# 8. Race-Specific Anatomy

## Human

Baseline compatibility race.

```text
standard humanoid head
standard shoulders
standard arm length
no special body appendages
```

Can use most humanoid equipment.

---

## Elf

Differences:

```text
slimmer torso
narrower shoulders
slightly longer arms
slightly larger or longer ears
smaller body mass
```

Important:

- helmets must either include ear clearance;
- or use elf-specific helmet variants;
- body armor may share a humanoid template if sufficiently loose;
- tight armor should use a slender variant.

---

## Dwarf

Differences:

```text
short body
broad torso
large head
short arms
large beard volume
wide shoulders
```

Important:

- normal humanoid chest armor should not simply be scaled down;
- armor must use a `ShortBroad` shell;
- helmets need extra head width;
- beard-compatible headwear must preserve beard volume;
- long weapons can remain universal, but hand placement may require positional offsets.

---

## Orc

Differences:

```text
large torso
wide shoulders
thicker arms
forward head posture
possible tusks
large jaw
```

Important:

- helmets need jaw/tusk clearance where required;
- shoulder equipment needs larger bounds;
- tight torso clothing should use `Humanoid_Large`;
- weapons may use universal grip standards.

---

## Catfolk

Differences:

```text
digitigrade lower body
tail
feline head
ears above head silhouette
possibly narrower torso
```

Important:

- body armor can often reuse humanoid torso families;
- lower-body clothing must account for tail;
- helmets require ear compatibility;
- back items must avoid clipping with tail and possibly high shoulder movement.

Define:

```text
TailClearanceRequired
EarClearanceRequired
```

---

## Birdfolk

Differences:

```text
beak
avian head
crest or feathers
possible wings or wing-like arms depending on design
different shoulder silhouette
```

Important:

- standard humanoid helmets usually should not be considered compatible;
- headwear should use `AvianHead`;
- chest armor may require a unique shell;
- backpacks require wing-clearance rules if wings are present.

---

## Lizardfolk

Differences:

```text
snout
tail
longer head
possible dorsal crest
different neck transition
```

Important:

- helmets require `ReptilianHead`;
- chest armor can potentially share humanoid-large profiles;
- robes and back items need tail clearance;
- neck equipment must account for longer head/neck geometry.

---

## Walrus / Heavy Non-Human

Differences:

```text
very broad torso
large head
short neck
large body depth
non-standard arms/body proportions
```

Important:

- do not attempt universal humanoid armor fitting;
- use race-specific clothing shells;
- weapons can still follow universal hand grip standards;
- headwear should be race-specific.

---

# 9. Equipment Compatibility Model

Each equipment asset should declare compatibility explicitly.

Example:

```cpp
struct EquipmentCompatibility
{
    BodyFamilyMask bodyFamilies;
    BodyProfileMask profiles;
    HeadTypeMask headTypes;

    bool requiresTailClearance;
    bool requiresEarClearance;
    bool requiresHornClearance;
    bool requiresWingClearance;
    bool beardCompatible;
};
```

Example asset metadata:

```json
{
  "name": "BronzeCuirass_HumanoidMedium",
  "slot": "Torso",
  "bodyFamilies": [
    "Humanoid_Medium",
    "Humanoid_Slender"
  ],
  "profiles": [
    "Average",
    "Slim"
  ],
  "tailClearance": false,
  "wingClearance": false
}
```

---

# 10. Equipment Categories

## 10.1 Weapons

Weapons should be the most universal asset type.

Examples:

```text
sword
axe
spear
club
mace
dagger
bow
javelin
sickle
tool
```

Standard:

```text
origin = primary grip point
+Z = weapon forward direction
+Y = weapon up
```

Optional child sockets:

```text
PrimaryGrip
SecondaryGrip
ProjectileSpawn
```

For two-handed weapons:

```text
PrimaryGrip
SecondaryGrip
```

The pawn system may position the second arm procedurally.

---

# 11. Shields

Shield standard:

```text
origin = grip point
local orientation = identical for all shields
```

Required metadata:

```text
radius / bounds
gripOffset
handSideCompatibility
```

Possible shield types:

```text
round
oval
tower
hide shield
wooden shield
bronze shield
```

Shields should be independent of race wherever hand proportions permit.

For large races, optional grip offsets can be stored in metadata.

---

# 12. Headwear

Headwear should be categorized by head compatibility rather than body compatibility.

Suggested head families:

```text
HumanoidHead
HumanoidWideHead
ElfHead
OrcHead
FelineHead
AvianHead
ReptilianHead
HeavyHead
```

Headwear metadata:

```text
headFamily
earClearance
hornClearance
tuskClearance
beardCompatible
hairSuppression
```

Example:

```json
{
  "name": "BronzeHelmet_Open",
  "headFamily": ["HumanoidHead", "ElfHead"],
  "earClearance": true,
  "beardCompatible": true,
  "hairSuppression": "Full"
}
```

---

# 13. Hair

Hair should be modular and categorized by head family.

Do not dynamically deform hair to fit arbitrary heads.

Use compatibility groups:

```text
Hair_Humanoid
Hair_Elf
Hair_Orc
Hair_Feline
Hair_Avian
Hair_Reptilian
```

Possible metadata:

```text
compatibleHeadFamily
helmetCompatibility
hideUnderHelmet
supportsBeard
```

---

# 14. Beards

Beards require explicit volume handling.

Especially important for dwarves.

Suggested categories:

```text
ShortBeard
MediumBeard
LargeBeard
BraidedBeard
ForkedBeard
```

Metadata:

```text
beardVolume
helmetCompatible
chestArmorClearance
```

Large dwarf beards may overlap chest armor intentionally, but clipping must remain within acceptable bounds.

---

# 15. Clothing Strategy

Do not treat clothing as cloth simulation.

Use rigid replacement shells.

Recommended:

```text
Body_Base
Body_Tunic
Body_Robe
Body_Leather
Body_Armor
Body_Priest
Body_Worker
```

Each clothing asset should replace or cover the torso body shell.

For tight-fitting clothes:

```text
use body-family-specific variant
```

For loose clothes:

```text
allow sharing across compatible profiles
```

---

# 16. Clothing Families

Suggested naming:

```text
Tunic_HM_Average
Tunic_HM_Broad
Tunic_HSB_Broad
Tunic_HL_Heavy
```

Where:

```text
HM  = Humanoid_Medium
HS  = Humanoid_Slender
HSB = Humanoid_ShortBroad
HL  = Humanoid_Large
```

Race-specific assets:

```text
Robe_Avian
Armor_Walrus
Helmet_Lizard
```

should be used where shared shells are impractical.

---

# 17. Long Clothing

Long robes, skirts, cloaks and capes are riskier.

Without skinning or cloth simulation:

### Robes

Prefer:

```text
single rigid lower-body shell
```

or:

```text
upper shell
+
lower rigid shell
```

### Cloaks

Use:

```text
short rigid cape
```

or:

```text
2–3 rigid segments
```

Avoid long dynamically deforming capes.

---

# 18. Back Items

Examples:

```text
backpack
quiver
bedroll
basket
shield-on-back
tool bundle
```

Use:

```text
Back_Center
```

as the main attachment socket.

Metadata should specify:

```text
tailClearance
wingClearance
headClearance
maxBounds
```

For races with wings or large tails, use race-specific variants.

---

# 19. Collision / Forbidden Volumes

Each race body template should provide simple forbidden volumes.

Example:

```text
HeadVolume
FaceVolume
TorsoVolume
ArmSweep_L
ArmSweep_R
TailSweep
WingSweep
```

Equipment generation can be automatically validated against these volumes.

Example:

```cpp
if (helmet.Intersects(FaceVolume))
    RejectAsset();

if (backpack.Intersects(WingSweep))
    RejectAsset();
```

These do not need precise mesh collision.

Simple capsules, spheres and boxes are sufficient.

---

# 20. Template Files

Create standard template files before generating content.

Recommended:

```text
templates/
├── Pawn_Human_Average.blend
├── Pawn_Elf_Slim.blend
├── Pawn_Dwarf_Broad.blend
├── Pawn_Orc_Large.blend
├── Pawn_Catfolk.blend
├── Pawn_Birdfolk.blend
├── Pawn_Lizardfolk.blend
└── Pawn_Walrus.blend
```

And:

```text
templates/equipment/
├── Weapon_Template.blend
├── Shield_Template.blend
├── Helmet_Template.blend
├── ClothingShell_Template.blend
├── Hair_Template.blend
├── Beard_Template.blend
└── BackItem_Template.blend
```

Agents should generate assets **inside these templates** rather than starting from an empty scene.

---

# 21. Agent Rules

When asking an agent to create an asset, always provide:

```text
target body family
target body profile
reference pawn file
equipment slot
attachment socket
allowed bounding box
triangle budget
material slot requirements
naming convention
export format
```

Example instruction:

```text
Create a Bronze Age bronze cuirass.

Target:
Humanoid_Medium / Average

Use the supplied Pawn_Human_Average reference.

Do not:
- move PawnRoot
- modify socket positions
- change unit scale
- modify the base pawn

Requirements:
- separate rigid mesh
- fit around Body
- no intersection with ArmSweep_L or ArmSweep_R
- under 180 triangles
- one material slot
- origin aligned to BodyRoot
- export as GLB
```

This is much more reliable than:

```text
Make a bronze armor for my character.
```

---

# 22. Naming Convention

Recommended format:

```text
<Category>_<Name>_<BodyFamily>_<Profile>_<Variant>
```

Examples:

```text
Armor_BronzeCuirass_HM_Average_A
Armor_Gambeson_HSB_Broad_A
Helmet_Kettle_HumanoidHead_A
Helmet_Bronze_AvianHead_A
Weapon_Khopesh_Universal_A
Shield_RoundWood_Universal_A
Hair_Braid_HumanoidHead_A
Beard_Long_DwarfHead_A
Back_Quiver_HM_Average_A
```

Universal items may omit body family:

```text
Weapon_Spear_Universal_A
```

---

# 23. Runtime Item Definition

Suggested structure:

```cpp
struct VisualEquipmentAsset
{
    AssetId mesh;
    EquipmentSlot slot;

    BodyFamilyMask compatibleBodies;
    HeadFamilyMask compatibleHeads;

    SocketId socket;

    Transform localTransform;

    bool hideHair;
    bool hideBeard;

    bool requiresTailClearance;
    bool requiresEarClearance;
    bool requiresHornClearance;
    bool requiresWingClearance;
};
```

---

# 24. Equipment Slots

Recommended initial slots:

```text
Head
Hair
Beard
Torso
Back
MainHand
OffHand
```

Possible future slots:

```text
Shoulders
Neck
Belt
Hip
Face
Tail
```

Keep the initial system small.

---

# 25. Material Strategy

Avoid unique materials for every item.

Use shared material families:

```text
Skin
Hair
Cloth
Leather
Wood
Bronze
Iron
Stone
Bone
```

Colors should preferably come from:

```text
instance parameters
palette index
mask texture
```

Example:

```text
clothPrimaryColor
clothSecondaryColor
metalTint
woodTint
```

This allows many visual variants without many materials.

---

# 26. Texture / Mask Strategy

For simple stylized pawns, prefer:

```text
shared atlas
+
color masks
+
small normal/roughness maps
```

Possible channels:

```text
R = primary recolor region
G = secondary recolor region
B = tertiary detail region
A = material mask / misc
```

This works well for clothing, shields and race variation.

---

# 27. LOD Rules for Equipment

Equipment should follow the pawn LOD.

Example:

## LOD0

```text
all visible equipment
full silhouette
```

## LOD1

```text
simplified hair
simplified beard
simplified weapon
simplified armor
```

## LOD2

```text
merge or omit tiny accessories
possibly omit beard
possibly simplify weapon to silhouette
```

At very long distance:

```text
pawn impostor
```

should include equipment only as part of the rendered silhouette.

---

# 28. Automated Validation

Every generated asset should be checked automatically.

Minimum validation:

```text
correct object names
correct unit scale
correct axis orientation
correct origin
triangle count
material count
bounding box
socket existence
forbidden volume intersection
export file exists
```

Possible output:

```text
PASS
WARNING
FAIL
```

Example:

```text
PASS: triangle count = 142
PASS: material slots = 1
PASS: origin matches Hand_R
WARNING: helmet within 2 mm of EarVolume
FAIL: intersects FaceVolume
```

---

# 29. Compatibility Philosophy

Do not force universal compatibility where it damages silhouettes.

Good sharing candidates:

```text
weapons
shields
small bags
tools
some jewelry
some loose clothing
```

Poor sharing candidates:

```text
tight chest armor
helmets
long robes
large backpacks
race-specific headwear
items affected by tails or wings
```

It is better to have:

```text
BronzeCuirass_Human
BronzeCuirass_Dwarf
BronzeCuirass_Orc
```

than one badly-scaled universal cuirass.

---

# 30. Suggested Initial Production Set

Build a small compatibility proof first.

## Races

```text
Human
Dwarf
Orc
Elf
```

## Equipment

```text
2 tunics
1 robe
1 leather armor
1 bronze cuirass

2 helmets
1 cap

1 sword
1 axe
1 spear
1 bow

1 round shield
1 backpack
1 quiver

3 hairstyles
3 beards
```

Validate that the system works before adding exotic races.

Then extend to:

```text
Catfolk
Birdfolk
Lizardfolk
Walrus
```

---

# 31. Final System Principle

The content pipeline should optimize for:

```text
strict compatibility
+
modular reuse
+
simple generation constraints
+
automatic validation
```

rather than:

```text
AI generates arbitrary meshes
and the runtime tries to make them fit
```

The correct workflow is:

```text
Race specification
        ↓
Body family
        ↓
Reference template
        ↓
Equipment compatibility contract
        ↓
Asset generation
        ↓
Automatic validation
        ↓
Runtime modular composition
```

This makes AI-generated content practical even across very different races and body proportions.
