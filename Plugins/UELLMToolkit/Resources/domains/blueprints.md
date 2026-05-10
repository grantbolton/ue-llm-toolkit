# Blueprints Domain — Blueprint & AnimBP Operations

## Node ID System

Created nodes get auto-generated IDs in `NodeComment`: `{NodeType}_{Context}_{Counter}`. These IDs are used by all subsequent operations: `connect_pins`, `delete_node`, `set_pin_value`, `get_node`, `verify_connection`, `get_node_connections`, `get_exec_chain`.

AnimGraph nodes use UE node name as fallback ID (e.g., `AnimGraphNode_SequencePlayer_0`). `get_graph_summary` and `find_nodes` auto-assign IDs to K2 nodes that lack them.

Cross-prefix lookup: `FindNodeById` checks `MCP_ANIM_ID:` (anim_blueprint_modify), `MCP_ID:` (blueprint_query), and raw UE node names — all callers benefit.

## Targeted Queries (Prefer Over Full Dumps)

Full graph dumps (`get_event_graph`, `get_anim_graph`) waste tokens on large BPs. Use targeted queries instead — 97% token savings on 100-node BPs.

| Operation | Purpose |
|-----------|---------|
| `find_nodes` | Search by class/name/comment |
| `get_node` | Single node + all pins |
| `verify_connection` | Boolean check between two pins |
| `get_node_connections` | One node's wiring |
| `get_graph_summary` | Census + auto-assigned IDs |
| `get_exec_chain` | Scoped walk from a starting node |

All work on both K2 (EventGraph) and AnimGraph nodes, including state-bound sub-graphs (`graph_type: "StateGraph"`).

## State-Bound Graphs

Nodes inside state machine states require `state_machine` + `state_name` parameters on:
- `bind_variable`
- `connect_anim_nodes`
- `layout_graph`

**Without these params, operations silently fail to find nodes.** After wiring, verify with `get_state_machine_detail` — unwired pins show `[UNWIRED=value]`.

State machine internal names differ from display names: `AnimationStateMachineGraph_1` vs `Locomotion`. Always query with `get_state_machine_detail` to get the internal name before modifying.

## AnimBP Creation From Scratch

**NEVER duplicate AnimBPs** — `asset duplicate` carries stale generated class references that cause unresolvable compile errors. Always build from scratch.

### Full Sequence

1. `blueprint_modify create` — `parent_class: "AnimInstance"` (creates without skeleton)
2. `asset set_asset_property` — `property: "TargetSkeleton"`, `value: "<skeleton_path>"` (required before anim_blueprint_modify ops work)
3. `anim_blueprint_modify add_layer_interface` — `interface_path` needs `_C` suffix (e.g., `.ALI_Locomotion_C`)
4. `blueprint_modify add_variable` x N — all variables needed by blend space bindings + transition conditions
5. `anim_blueprint_modify create_state_machine` — `target_graph: "LayerName"` (for layer graphs)
6. `anim_blueprint_modify connect_state_machine_to_output` — pass `target_graph` when SM is in a layer function graph (without it, wires to main AnimGraph — wrong graph)
7. `anim_blueprint_modify add_state` x N — `is_entry_state: true` on the first
8. `anim_blueprint_modify set_state_animation` — with `parameter_bindings` for blend spaces
9. `anim_blueprint_modify add_transition` x N — **duration param is ignored, always defaults to 0.2s**
10. `set_transition_duration` — set correct durations after creation
11. `anim_blueprint_modify add_comparison_chain` — one per condition per transition

### Broken Operations (Avoid)
- `setup_transition_conditions` — `type`/`comparison_type` fields are broken. Use `add_comparison_chain` instead.
- `asset duplicate` on AnimBPs or any Blueprint with complex internal references — stale class identity causes compile errors.

## layout_graph

BFS auto-layout operation on `blueprint_modify` and `anim_blueprint_modify`.

| Parameter | Default | Notes |
|-----------|---------|-------|
| `spacing_x` | 400 | Horizontal gap |
| `spacing_y` | 200 | Vertical gap |
| `preserve_existing` | false | Keep current positions |

AnimBP variant also takes `state_machine` + `state_name`. K2 graphs: events left, flow right. AnimGraph: sources left, Output Pose right.

**Use after building a new graph, not after every node add.**

## CDO Defaults

- Enum values require fully qualified names: `EMyEnum::ValueName` (not just `ValueName`). Numeric index also works.
- `set_cdo_default` only affects future spawns. Actors already placed in a level have serialized values in the `.umap` — CDO changes are invisible to them. Fix: also `set_property` on the level actor, or delete and re-place.

## Blend Space Parameter Binding

`set_state_animation` with `parameter_bindings` works — bindings are applied automatically after BS node creation.

1D BlendSpace pin name: always `"X"` (hardcoded in the plugin, regardless of axis display name).

2D BlendSpace: use the axis display names from the blend space asset.

## Common Patterns

### _C Suffix for Class Paths
Blueprint class paths (for `spawn_actor`, `parent_class`, etc.) need the `_C` suffix:
```
/Game/Blueprints/BP_MyActor.BP_MyActor_C
```
Asset paths (for `blueprint_query`, `asset` operations) do NOT use `_C`.

### Transition Duration Workaround
`add_transition` always creates with 0.2s duration regardless of params. After creating all transitions, call `set_transition_duration` separately for each one.

### Layer Interface Notes
- `add_layer_interface` adds existing ALIs. No `create_layer_interface` yet.
- After `add_layer_interface`, AnimBP generated class is stale — recompile before `list_layers`.
- `add_linked_layer_node` without matching ALI creates self-layer; with ALI present, auto-references interface.

### Blend Space Axis Interpolation
Set via `blend_space set_axis`:
- `interp_type: "SpringDamper"` — most natural-feeling
- `max_speed` — hard cap on axis velocity (most impactful single knob)
- `interp_time` — transition window duration
- `damping_ratio` — 0.85 = slight springy overshoot (organic), 1.0 = critically damped

## Gameplay BP Authoring (Components, Instance Methods, Hit Events)

The toolkit handles the full SCS-component → instance-method → component-bound-event idiom in `blueprint_modify`. Bouncy-cube example:

```jsonc
// 1. Create the BP
{ "operation": "create", "package_path": "/Game/Blueprints", "blueprint_name": "BP_BouncyCube", "parent_class": "Actor" }

// 2. Add an SCS component
{ "operation": "add_component", "blueprint_path": "/Game/Blueprints/BP_BouncyCube",
  "component_class": "StaticMeshComponent", "component_name": "Cube" }

// 3. Enable physics on the component (recompile happens automatically)
{ "operation": "set_component_default", "blueprint_path": "/Game/Blueprints/BP_BouncyCube",
  "component_name": "Cube", "property": "BodyInstance.bSimulatePhysics", "value": true }

// 4. Bind to OnComponentHit on the SCS component
{ "operation": "add_node", "blueprint_path": "/Game/Blueprints/BP_BouncyCube",
  "node_type": "ComponentBoundEvent",
  "node_params": { "component": "Cube", "delegate": "OnComponentHit" } }

// 5. Reference the SCS component as a graph variable
{ "operation": "add_node", "blueprint_path": "/Game/Blueprints/BP_BouncyCube",
  "node_type": "VariableGet", "node_params": { "variable": "Cube" } }

// 6. Call AddImpulse on UPrimitiveComponent (any UCLASS now resolves)
{ "operation": "add_node", "blueprint_path": "/Game/Blueprints/BP_BouncyCube",
  "node_type": "CallFunction",
  "node_params": { "function": "AddImpulse", "target_class": "PrimitiveComponent" } }
```

`target_class` accepts bare names (engine `U`/`A` prefix is auto-retried) and any UCLASS in any module — not just `KismetSystemLibrary` / `KismetMathLibrary` / `KismetStringLibrary` / `AnimInstance` / `GameplayStatics`. `VariableGet` sees SCS components added via `add_component` (looks them up on `SkeletonGeneratedClass`).

### Other unlocked node types

| `node_type` | `node_params` | Notes |
|-----|-----|-----|
| `CustomEvent` | `event_name`, optional `inputs: [{name, type}]` | Types: bool, int32, float, double, FString, FName, FVector, FRotator |
| `ComponentBoundEvent` | `component`, `delegate` | e.g. `OnComponentHit`, `OnComponentBeginOverlap`, `OnComponentEndOverlap` |
| `Self` | — | Reference to the BP instance; wire to `self` pins for instance method calls |
| `Cast` (or `DynamicCast`) | `target_class` | Bare names work — engine prefix auto-retried |
| `MakeStruct` | `struct_type` | e.g. `Vector`, `LinearColor`, `HitResult`, `Transform` (`F` prefix auto-retried) |
| `BreakStruct` | `struct_type` | Same |
