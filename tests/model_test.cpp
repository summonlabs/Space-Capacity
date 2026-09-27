// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

// Space Capacity - kinds, containment, lifecycle and node shape.
//
// What this file proves:
//   * the containment table is exactly the documented 7x7 table, including that
//     nothing contains a site and nothing sits inside a rack unit band, and that
//     a level may be skipped;
//   * canonical depth runs 0..5 from a site to a rack unit band, and the
//     spatial class admitted for each kind is exactly the documented set;
//   * the three lifecycle predicates are disjoint and take exactly the
//     documented values;
//   * lifecycle transitions are exactly the documented table, with no self
//     transitions, `replaced` terminal and `retired` re-entering only through
//     `provisioning`;
//   * a well-formed node of every kind validates, and each single broken field
//     is refused with its documented error code;
//   * containment rules hold for the fixture's own chain and for a skipped
//     level, and node equality is reflexive and sensitive to every field.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dccp/space_capacity/error.hpp"
#include "dccp/space_capacity/evidence.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/model.hpp"
#include "dccp/space_capacity/registry_ref.hpp"
#include "dccp/space_capacity/units.hpp"

#include "fixture.hpp"
#include "test_support.hpp"

using namespace dccp::space_capacity;

namespace {

constexpr SpaceNodeKind kAllKinds[] = {SpaceNodeKind::site,      SpaceNodeKind::building,
                                       SpaceNodeKind::hall,      SpaceNodeKind::row,
                                       SpaceNodeKind::rack,      SpaceNodeKind::rack_unit_band};

void expect_status(const Status& status, ErrorCode expected) {
  SC_CHECK(!status.ok());
  SC_CHECK_EQ(status.code(), expected);
}

// One well-formed node of each kind, built through the shared fixture's node
// constructor so that every test in this file works on the same shape as the
// rest of the suite. The chain is the fixture's own:
// site-a <- bldg-1 <- hall-1 <- row-1 <- rack-01 <- band-a.
[[nodiscard]] SpaceNode good_node(SpaceNodeKind kind) {
  const sc_fixture::Ids id = sc_fixture::ids();
  switch (kind) {
    case SpaceNodeKind::site:
      return sc_fixture::make_node(id.site, kind, SpatialClass::outdoor, SpaceNodeId{}, 0, "Site A");
    case SpaceNodeKind::building: {
      SpaceNode node =
          sc_fixture::make_node(id.building, kind, SpatialClass::enclosed, id.site, 1, "Building 1");
      node.own_planar.declared_area = sc_fixture::building_area();
      node.own_planar.rects = *RectSet::build({PlanarRect::make(0, 0, sc_fixture::kBuildingWidth,
                                                                 sc_fixture::kBuildingDepth)});
      return node;
    }
    case SpaceNodeKind::hall: {
      SpaceNode node =
          sc_fixture::make_node(id.hall, kind, SpatialClass::floor, id.building, 2, "Hall 1");
      node.own_planar.declared_area = sc_fixture::hall_area();
      node.own_planar.rects = *RectSet::build(
          {PlanarRect::make(0, 0, sc_fixture::kHallWidth, sc_fixture::kHallDepth)});
      node.placement.has_base_rect = true;
      node.placement.base_rect =
          PlanarRect::make(0, 0, sc_fixture::kHallWidth, sc_fixture::kHallDepth);
      return node;
    }
    case SpaceNodeKind::row:
      return sc_fixture::make_node(id.row1, kind, SpatialClass::aisle, id.hall, 3, "Row 1");
    case SpaceNodeKind::rack: {
      SpaceNode node = sc_fixture::make_node(id.rack1, kind, SpatialClass::rack, id.row1, 4, "Rack");
      node.own_rack.height = RackUnits{sc_fixture::kRackHeight};
      node.placement.has_base_rect = true;
      node.placement.base_rect =
          PlanarRect::make(0, 0, sc_fixture::kRackWidth, sc_fixture::kRackDepth);
      return node;
    }
    case SpaceNodeKind::rack_unit_band: {
      SpaceNode node =
          sc_fixture::make_node(id.band, kind, SpatialClass::rack, id.rack1, 5, "band");
      node.placement.has_u_span = true;
      node.placement.u_span = RackUnitInterval::of_count(1, 4);
      return node;
    }
    case SpaceNodeKind::none:
      break;
  }
  return SpaceNode{};
}

[[nodiscard]] EvidenceSet one_evidence() {
  EvidenceRef reference;
  reference.source = RegistryKind::rack_registry;
  reference.id = *ExternalId::parse("rack:a01");
  reference.generation = EntityGeneration{1};
  reference.observed_at = AttemptId{StoreIncarnation{1}, 1};
  const Result<EvidenceSet> built = EvidenceSet::build({reference});
  if (!built.ok()) return EvidenceSet{};
  return built.value();
}

// ---------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------

void kind_containment_table() {
  SC_CASE("kind_may_contain matches the documented 7x7 table");
  // Rows are the parent kind, columns the child kind, both in enum order:
  // none, site, building, hall, row, rack, rack_unit_band.
  const bool expected[kSpaceNodeKindCount][kSpaceNodeKindCount] = {
      /* none           */ {false, false, false, false, false, false, false},
      /* site           */ {false, false, true, true, true, true, false},
      /* building       */ {false, false, false, true, true, true, false},
      /* hall           */ {false, false, false, false, true, true, false},
      /* row            */ {false, false, false, false, false, true, false},
      /* rack           */ {false, false, false, false, false, false, true},
      /* rack_unit_band */ {false, false, false, false, false, false, false},
  };
  for (std::size_t parent = 0; parent < kSpaceNodeKindCount; ++parent) {
    for (std::size_t child = 0; child < kSpaceNodeKindCount; ++child) {
      const auto parent_kind = static_cast<SpaceNodeKind>(parent);
      const auto child_kind = static_cast<SpaceNodeKind>(child);
      SC_CHECK_EQ(kind_may_contain(parent_kind, child_kind), expected[parent][child]);
    }
  }

  // The named rows of the table.
  SC_CHECK(kind_may_contain(SpaceNodeKind::site, SpaceNodeKind::building));
  SC_CHECK(kind_may_contain(SpaceNodeKind::site, SpaceNodeKind::hall));
  SC_CHECK(kind_may_contain(SpaceNodeKind::site, SpaceNodeKind::row));
  SC_CHECK(kind_may_contain(SpaceNodeKind::site, SpaceNodeKind::rack));
  SC_CHECK(kind_may_contain(SpaceNodeKind::building, SpaceNodeKind::hall));
  SC_CHECK(kind_may_contain(SpaceNodeKind::building, SpaceNodeKind::row));
  SC_CHECK(kind_may_contain(SpaceNodeKind::building, SpaceNodeKind::rack));
  SC_CHECK(kind_may_contain(SpaceNodeKind::hall, SpaceNodeKind::row));
  SC_CHECK(kind_may_contain(SpaceNodeKind::hall, SpaceNodeKind::rack));
  SC_CHECK(kind_may_contain(SpaceNodeKind::row, SpaceNodeKind::rack));
  SC_CHECK(kind_may_contain(SpaceNodeKind::rack, SpaceNodeKind::rack_unit_band));
  SC_CHECK(!kind_may_contain(SpaceNodeKind::site, SpaceNodeKind::site));
  SC_CHECK(!kind_may_contain(SpaceNodeKind::site, SpaceNodeKind::rack_unit_band));
  SC_CHECK(!kind_may_contain(SpaceNodeKind::building, SpaceNodeKind::site));
  SC_CHECK(!kind_may_contain(SpaceNodeKind::hall, SpaceNodeKind::building));

  for (std::size_t index = 0; index < kSpaceNodeKindCount; ++index) {
    const auto kind = static_cast<SpaceNodeKind>(index);
    SC_CHECK(!kind_may_contain(kind, SpaceNodeKind::site));  // nothing contains a site
    SC_CHECK(!kind_may_contain(SpaceNodeKind::rack_unit_band, kind));
    SC_CHECK(!kind_may_contain(SpaceNodeKind::none, kind));
    SC_CHECK(!kind_may_contain(kind, SpaceNodeKind::none));
  }
}

void canonical_depths_and_classes() {
  SC_CASE("canonical_depth is 0..5 and classes are per kind");
  SC_CHECK_EQ(canonical_depth(SpaceNodeKind::site), std::uint32_t{0});
  SC_CHECK_EQ(canonical_depth(SpaceNodeKind::building), std::uint32_t{1});
  SC_CHECK_EQ(canonical_depth(SpaceNodeKind::hall), std::uint32_t{2});
  SC_CHECK_EQ(canonical_depth(SpaceNodeKind::row), std::uint32_t{3});
  SC_CHECK_EQ(canonical_depth(SpaceNodeKind::rack), std::uint32_t{4});
  SC_CHECK_EQ(canonical_depth(SpaceNodeKind::rack_unit_band), std::uint32_t{5});

  SC_CHECK(kind_may_be_root(SpaceNodeKind::site));
  for (const SpaceNodeKind kind : kAllKinds) {
    SC_CHECK_EQ(kind_may_be_root(kind), kind == SpaceNodeKind::site);
  }
  SC_CHECK(!kind_may_be_root(SpaceNodeKind::none));

  // Which kinds may declare an envelope, a placement rectangle or a span.
  SC_CHECK(kind_may_declare_plane(SpaceNodeKind::site));
  SC_CHECK(kind_may_declare_plane(SpaceNodeKind::building));
  SC_CHECK(kind_may_declare_plane(SpaceNodeKind::hall));
  SC_CHECK(kind_may_declare_plane(SpaceNodeKind::row));
  SC_CHECK(!kind_may_declare_plane(SpaceNodeKind::rack));
  SC_CHECK(!kind_may_declare_plane(SpaceNodeKind::rack_unit_band));
  for (const SpaceNodeKind kind : kAllKinds) {
    SC_CHECK_EQ(kind_may_declare_rack_envelope(kind), kind == SpaceNodeKind::rack);
    SC_CHECK_EQ(kind_may_have_u_span(kind), kind == SpaceNodeKind::rack_unit_band);
  }
  SC_CHECK(!kind_may_have_base_rect(SpaceNodeKind::site));
  SC_CHECK(kind_may_have_base_rect(SpaceNodeKind::building));
  SC_CHECK(kind_may_have_base_rect(SpaceNodeKind::hall));
  SC_CHECK(kind_may_have_base_rect(SpaceNodeKind::row));
  SC_CHECK(kind_may_have_base_rect(SpaceNodeKind::rack));
  SC_CHECK(!kind_may_have_base_rect(SpaceNodeKind::rack_unit_band));

  // Columns are SpatialClass values 0..8: unspecified, floor, rack, aisle,
  // overhead, outdoor, service, enclosed, cage.
  const bool allowed[kSpaceNodeKindCount][9] = {
      /* none           */ {false, false, false, false, false, false, false, false, false},
      /* site           */ {false, true, false, false, false, true, false, false, false},
      /* building       */ {false, true, false, false, false, false, false, true, false},
      /* hall           */ {false, true, false, true, false, false, false, true, true},
      /* row            */ {false, true, false, true, false, false, false, false, false},
      /* rack           */ {false, false, true, false, false, false, false, false, false},
      /* rack_unit_band */ {false, false, true, false, false, false, false, false, false},
  };
  for (std::size_t kind = 0; kind < kSpaceNodeKindCount; ++kind) {
    for (std::size_t cls = 0; cls < 9; ++cls) {
      SC_CHECK_EQ(spatial_class_allowed(static_cast<SpaceNodeKind>(kind),
                                        static_cast<SpatialClass>(cls)),
                  allowed[kind][cls]);
    }
    // `unspecified` is refused for every kind, including `none`.
    SC_CHECK(!spatial_class_allowed(static_cast<SpaceNodeKind>(kind), SpatialClass::unspecified));
  }
  SC_CHECK(spatial_class_allowed(SpaceNodeKind::rack, SpatialClass::rack));
  SC_CHECK(!spatial_class_allowed(SpaceNodeKind::rack, SpatialClass::floor));
  SC_CHECK(!spatial_class_allowed(SpaceNodeKind::building, SpatialClass::cage));
  SC_CHECK(spatial_class_allowed(SpaceNodeKind::hall, SpatialClass::cage));
  SC_CHECK(spatial_class_allowed(SpaceNodeKind::hall, SpatialClass::aisle));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void lifecycle_truth_table() {
  SC_CASE("the three lifecycle predicates take exactly the documented values");
  struct LifecycleCase final {
    NodeLifecycle value;
    bool usable_now;
    bool planned;
    bool terminal;
  };
  const LifecycleCase table[] = {
      {NodeLifecycle::planned, false, true, false},
      {NodeLifecycle::provisioning, false, true, false},
      {NodeLifecycle::available, true, false, false},
      {NodeLifecycle::restricted, true, false, false},
      {NodeLifecycle::decommissioning, false, false, false},
      {NodeLifecycle::retired, false, false, true},
      {NodeLifecycle::replaced, false, false, true},
  };
  for (const LifecycleCase& item : table) {
    SC_CHECK_EQ(lifecycle_is_usable_now(item.value), item.usable_now);
    SC_CHECK_EQ(lifecycle_is_planned(item.value), item.planned);
    SC_CHECK_EQ(lifecycle_is_terminal(item.value), item.terminal);
    const int true_count =
        (item.usable_now ? 1 : 0) + (item.planned ? 1 : 0) + (item.terminal ? 1 : 0);
    SC_CHECK(true_count <= 1);  // no state is in two of the three sets
  }
  // The three sets are disjoint, and `decommissioning` is in none of them: the
  // header says such a node "contributes nothing usable and says so". (The brief
  // that motivated this file said the three predicates partition the seven
  // states; disjointness holds, coverage does not, and the library's own wording
  // for `decommissioning` is the one asserted here.)
  SC_CHECK(!lifecycle_is_usable_now(NodeLifecycle::decommissioning));
  SC_CHECK(!lifecycle_is_planned(NodeLifecycle::decommissioning));
  SC_CHECK(!lifecycle_is_terminal(NodeLifecycle::decommissioning));
  SC_CHECK(lifecycle_is_usable_now(NodeLifecycle::available));
  SC_CHECK(lifecycle_is_terminal(NodeLifecycle::replaced));

  // Provenance of the values: every state round-trips through its raw value.
  const NodeLifecycle states[] = {NodeLifecycle::planned,     NodeLifecycle::provisioning,
                                  NodeLifecycle::available,   NodeLifecycle::restricted,
                                  NodeLifecycle::decommissioning, NodeLifecycle::retired,
                                  NodeLifecycle::replaced};
  for (const NodeLifecycle state : states) {
    const auto raw = static_cast<std::uint8_t>(state);
    SC_CHECK_EQ(node_lifecycle_from_value(raw), state);
    NodeLifecycle parsed = NodeLifecycle::planned;
    SC_CHECK(parse_node_lifecycle(node_lifecycle_name(state), parsed));
    SC_CHECK_EQ(parsed, state);
  }
}

void lifecycle_transition_table() {
  SC_CASE("lifecycle_transition_allowed matches the documented table");
  const NodeLifecycle order[] = {
      NodeLifecycle::planned,        NodeLifecycle::provisioning, NodeLifecycle::available,
      NodeLifecycle::restricted,     NodeLifecycle::decommissioning, NodeLifecycle::retired,
      NodeLifecycle::replaced};
  const bool expected[7][7] = {
      /* planned         */ {false, true, true, false, false, true, false},
      /* provisioning    */ {true, false, true, false, false, true, false},
      /* available       */ {false, false, false, true, true, true, true},
      /* restricted      */ {false, false, true, false, true, true, true},
      /* decommissioning */ {false, false, true, false, false, true, true},
      /* retired         */ {false, true, false, false, false, false, false},
      /* replaced        */ {false, false, false, false, false, false, false},
  };
  for (std::size_t from = 0; from < 7; ++from) {
    for (std::size_t to = 0; to < 7; ++to) {
      SC_CHECK_EQ(lifecycle_transition_allowed(order[from], order[to]), expected[from][to]);
    }
  }
  for (const NodeLifecycle state : order) {
    // A self transition is never a transition.
    SC_CHECK(!lifecycle_transition_allowed(state, state));
    // `replaced` is terminal.
    SC_CHECK(!lifecycle_transition_allowed(NodeLifecycle::replaced, state));
    // `retired` may only go to `provisioning`.
    SC_CHECK_EQ(lifecycle_transition_allowed(NodeLifecycle::retired, state),
                state == NodeLifecycle::provisioning);
  }
  SC_CHECK_EQ(lifecycle_transition_refusal(NodeLifecycle::available, NodeLifecycle::available),
              std::string_view("the node is already in that lifecycle state"));
  SC_CHECK_EQ(lifecycle_transition_refusal(NodeLifecycle::replaced, NodeLifecycle::available),
              std::string_view("a replaced node is terminal"));
}

// ---------------------------------------------------------------------------
// Shape validation
// ---------------------------------------------------------------------------

void good_nodes_validate() {
  SC_CASE("validate_node_shape accepts a well-formed node of every kind");
  for (const SpaceNodeKind kind : kAllKinds) {
    const SpaceNode node = good_node(kind);
    const Status status = validate_node_shape(node);
    SC_CHECK(status.ok());
    SC_CHECK_EQ(status.code(), ErrorCode::ok);
  }
  // A default-constructed node has no identity at all.
  expect_status(validate_node_shape(SpaceNode{}), ErrorCode::empty_value);
}

void node_shape_refusals() {
  SC_CASE("validate_node_shape refuses each single broken field");
  // No identity.
  SpaceNode broken = good_node(SpaceNodeKind::hall);
  broken.id = SpaceNodeId{};
  expect_status(validate_node_shape(broken), ErrorCode::empty_value);

  // No kind.
  broken = good_node(SpaceNodeKind::site);
  broken.kind = SpaceNodeKind::none;
  expect_status(validate_node_shape(broken), ErrorCode::invalid_kind_for_parent);

  // `unspecified` is not a class, for any kind.
  for (const SpaceNodeKind kind : kAllKinds) {
    SpaceNode probe = good_node(kind);
    probe.spatial_class = SpatialClass::unspecified;
    expect_status(validate_node_shape(probe), ErrorCode::invalid_spatial_class);
  }
  // A class the kind does not admit.
  broken = good_node(SpaceNodeKind::rack);
  broken.spatial_class = SpatialClass::floor;
  expect_status(validate_node_shape(broken), ErrorCode::invalid_spatial_class);
  broken = good_node(SpaceNodeKind::building);
  broken.spatial_class = SpatialClass::cage;
  expect_status(validate_node_shape(broken), ErrorCode::invalid_spatial_class);

  // A depth past the containment bound.
  broken = good_node(SpaceNodeKind::hall);
  broken.depth = Limits::kMaxContainmentDepth + 1;
  expect_status(validate_node_shape(broken), ErrorCode::depth_exceeded);

  // A root that is not a site.
  broken = good_node(SpaceNodeKind::building);
  broken.parent = SpaceNodeId{};
  broken.depth = 0;
  expect_status(validate_node_shape(broken), ErrorCode::kind_must_have_parent);

  // A root whose depth is not zero.
  broken = good_node(SpaceNodeKind::site);
  broken.depth = 1;
  expect_status(validate_node_shape(broken), ErrorCode::invalid_placement);

  // A contained node at depth zero.
  broken = good_node(SpaceNodeKind::hall);
  broken.depth = 0;
  expect_status(validate_node_shape(broken), ErrorCode::invalid_placement);

  // A plane on a rack.
  broken = good_node(SpaceNodeKind::rack);
  broken.own_planar.declared_area = SquareMillimeters{1000};
  expect_status(validate_node_shape(broken), ErrorCode::plane_not_declared);

  // A rack envelope on a hall.
  broken = good_node(SpaceNodeKind::hall);
  broken.own_rack.height = RackUnits{42};
  expect_status(validate_node_shape(broken), ErrorCode::envelope_not_declared);

  // A negative declared area.
  broken = good_node(SpaceNodeKind::hall);
  broken.own_planar.declared_area = SquareMillimeters{-1};
  broken.own_planar.rects = RectSet{};
  expect_status(validate_node_shape(broken), ErrorCode::invalid_extent);

  // A declared area past the bound.
  broken = good_node(SpaceNodeKind::hall);
  broken.own_planar.declared_area = SquareMillimeters{Limits::kMaxSquareMillimeters + 1};
  broken.own_planar.rects = RectSet{};
  expect_status(validate_node_shape(broken), ErrorCode::extent_out_of_envelope);

  // Rectangles whose total exceeds the declared area.
  broken = good_node(SpaceNodeKind::hall);
  broken.own_planar.declared_area = SquareMillimeters{100};
  broken.own_planar.rects = *RectSet::build({PlanarRect::make(0, 0, 1000, 1000)});
  expect_status(validate_node_shape(broken), ErrorCode::extent_out_of_envelope);

  // A rack height above the bound, and one below the minimum.
  broken = good_node(SpaceNodeKind::rack);
  broken.own_rack.height = RackUnits{Limits::kMaxRackUnits + 1};
  expect_status(validate_node_shape(broken), ErrorCode::unit_envelope_exceeded);
  broken = good_node(SpaceNodeKind::rack);
  broken.own_rack.height = RackUnits{-1};
  expect_status(validate_node_shape(broken), ErrorCode::unit_envelope_exceeded);
  // A height of exactly the bound is accepted.
  SpaceNode tallest = good_node(SpaceNodeKind::rack);
  tallest.own_rack.height = RackUnits{Limits::kMaxRackUnits};
  SC_CHECK(validate_node_shape(tallest).ok());

  // A rack height of zero is not an out-of-range height: `is_declared` is
  // "height is not zero", so zero is the absent value, exactly as it is for a
  // grouping node that declares no rack envelope at all. (The brief that
  // motivated this file expected a refusal here; the library's own definition of
  // a declared envelope, and the mutation path, treat a zero height as clearing
  // the envelope. What is refused is a declared height outside
  // [1, kMaxRackUnits], asserted immediately above.)
  SpaceNode undeclared = good_node(SpaceNodeKind::rack);
  undeclared.own_rack.height = RackUnits{0};
  SC_CHECK(!undeclared.own_rack.is_declared());
  SC_CHECK(validate_node_shape(undeclared).ok());

  // A base rectangle on a site, and a rack unit span on a site.
  broken = good_node(SpaceNodeKind::site);
  broken.placement.has_base_rect = true;
  broken.placement.base_rect = PlanarRect::make(0, 0, 10, 10);
  expect_status(validate_node_shape(broken), ErrorCode::invalid_placement);
  broken = good_node(SpaceNodeKind::site);
  broken.placement.has_u_span = true;
  broken.placement.u_span = RackUnitInterval::make(1, 2);
  expect_status(validate_node_shape(broken), ErrorCode::invalid_placement);

  // A placement rectangle on a kind that may not carry one.
  broken = good_node(SpaceNodeKind::rack_unit_band);
  broken.placement.has_base_rect = true;
  broken.placement.base_rect = PlanarRect::make(0, 0, 10, 10);
  expect_status(validate_node_shape(broken), ErrorCode::invalid_placement);

  // A degenerate placement rectangle.
  broken = good_node(SpaceNodeKind::hall);
  broken.placement.base_rect = PlanarRect::make(0, 0, 0, 10);
  expect_status(validate_node_shape(broken), ErrorCode::invalid_extent);

  // A rack unit span outside [1, kMaxRackUnits + 1).
  broken = good_node(SpaceNodeKind::rack_unit_band);
  broken.placement.u_span = RackUnitInterval::make(0, 2);
  expect_status(validate_node_shape(broken), ErrorCode::invalid_extent);
  broken = good_node(SpaceNodeKind::rack_unit_band);
  broken.placement.u_span = RackUnitInterval::make(1, Limits::kMaxRackUnits + 2);
  expect_status(validate_node_shape(broken), ErrorCode::invalid_extent);
  // The full envelope is a legal span.
  SpaceNode full_span = good_node(SpaceNodeKind::rack_unit_band);
  full_span.placement.u_span = RackUnitInterval::make(1, Limits::kMaxRackUnits + 1);
  SC_CHECK(validate_node_shape(full_span).ok());

  // Lineage: `replaced` needs a successor, a successor needs `replaced`, and a
  // node may not be its own successor.
  broken = good_node(SpaceNodeKind::hall);
  broken.lifecycle = NodeLifecycle::replaced;
  expect_status(validate_node_shape(broken), ErrorCode::lineage_broken);
  broken = good_node(SpaceNodeKind::hall);
  broken.replaced_by = *SpaceNodeId::parse("hall-2");
  expect_status(validate_node_shape(broken), ErrorCode::lineage_broken);
  broken = good_node(SpaceNodeKind::hall);
  broken.lifecycle = NodeLifecycle::replaced;
  broken.replaced_by = broken.id;
  expect_status(validate_node_shape(broken), ErrorCode::self_reference);
  broken = good_node(SpaceNodeKind::hall);
  broken.replaces = broken.id;
  expect_status(validate_node_shape(broken), ErrorCode::self_reference);
  broken = good_node(SpaceNodeKind::hall);
  broken.lifecycle = NodeLifecycle::replaced;
  broken.replaced_by = *SpaceNodeId::parse("hall-2");
  broken.replaces = *SpaceNodeId::parse("hall-2");
  expect_status(validate_node_shape(broken), ErrorCode::replacement_cycle);
  // A well-formed lineage is accepted.
  SpaceNode succeeded = good_node(SpaceNodeKind::hall);
  succeeded.lifecycle = NodeLifecycle::replaced;
  succeeded.replaced_by = *SpaceNodeId::parse("hall-2");
  succeeded.generation = EntityGeneration{2};
  SC_CHECK(validate_node_shape(succeeded).ok());

  // A rack registry reference on a kind that is not a rack.
  broken = good_node(SpaceNodeKind::hall);
  broken.rack = RackRef::of("rack:a01").value();
  expect_status(validate_node_shape(broken), ErrorCode::incompatible_kind);
  // The same reference on a rack is exactly right.
  SpaceNode rack_node = good_node(SpaceNodeKind::rack);
  rack_node.rack = RackRef::of("rack:a01").value();
  SC_CHECK(validate_node_shape(rack_node).ok());
  // An empty reference is malformed wherever it appears.
  broken = good_node(SpaceNodeKind::rack);
  broken.rack = RackRef{};
  expect_status(validate_node_shape(broken), ErrorCode::malformed_reference);
  broken = good_node(SpaceNodeKind::hall);
  broken.location = LocationRef{};
  expect_status(validate_node_shape(broken), ErrorCode::malformed_reference);
  // A populated location and facility node reference are ordinary.
  SpaceNode referenced = good_node(SpaceNodeKind::hall);
  referenced.location = LocationRef::of("loc-1").value();
  referenced.facility_node = FacilityNodeRef::of("fn-1").value();
  SC_CHECK(validate_node_shape(referenced).ok());
}

// ---------------------------------------------------------------------------
// Containment
// ---------------------------------------------------------------------------

void containment_rules() {
  SC_CASE("validate_containment");
  const SpaceNode site = good_node(SpaceNodeKind::site);
  const SpaceNode building = good_node(SpaceNodeKind::building);
  const SpaceNode hall = good_node(SpaceNodeKind::hall);
  const SpaceNode row = good_node(SpaceNodeKind::row);
  const SpaceNode rack = good_node(SpaceNodeKind::rack);
  const SpaceNode band = good_node(SpaceNodeKind::rack_unit_band);

  // The fixture's own chain is consistent at every level.
  SC_CHECK_EQ(validate_containment(site, building).code(), ErrorCode::ok);
  SC_CHECK_EQ(validate_containment(building, hall).code(), ErrorCode::ok);
  SC_CHECK_EQ(validate_containment(hall, row).code(), ErrorCode::ok);
  SC_CHECK_EQ(validate_containment(row, rack).code(), ErrorCode::ok);
  SC_CHECK_EQ(validate_containment(rack, band).code(), ErrorCode::ok);

  // Levels may be skipped: a rack directly under a site is legal containment
  // according to kind_may_contain, and validates when the depth is consistent.
  SpaceNode shallow_rack = rack;
  shallow_rack.parent = site.id;
  shallow_rack.depth = 1;
  SC_CHECK(kind_may_contain(SpaceNodeKind::site, SpaceNodeKind::rack));
  SC_CHECK(validate_node_shape(shallow_rack).ok());
  SC_CHECK_EQ(validate_containment(site, shallow_rack).code(), ErrorCode::ok);

  // Self containment.
  expect_status(validate_containment(site, site), ErrorCode::self_reference);
  // An illegal kind pair.
  SpaceNode nested_site = site;
  nested_site.id = *SpaceNodeId::parse("site-2");
  nested_site.parent = rack.id;
  nested_site.depth = rack.depth + 1;
  expect_status(validate_containment(rack, nested_site), ErrorCode::invalid_kind_for_parent);
  expect_status(validate_containment(hall, band), ErrorCode::invalid_kind_for_parent);
  // A depth that is not the parent depth plus one.
  SpaceNode wrong_depth = building;
  wrong_depth.depth = 3;
  expect_status(validate_containment(site, wrong_depth), ErrorCode::invalid_placement);
  SpaceNode too_deep = rack;
  too_deep.depth = 9;
  expect_status(validate_containment(site, too_deep), ErrorCode::invalid_placement);
  // A consistent but out-of-bounds depth is a depth failure.
  SpaceNode deep_parent = rack;
  deep_parent.depth = Limits::kMaxContainmentDepth;
  SpaceNode deep_child = band;
  deep_child.parent = deep_parent.id;
  deep_child.depth = Limits::kMaxContainmentDepth + 1;
  expect_status(validate_containment(deep_parent, deep_child), ErrorCode::depth_exceeded);
  // A missing identity on either side.
  SpaceNode anonymous = rack;
  anonymous.id = SpaceNodeId{};
  expect_status(validate_containment(site, anonymous), ErrorCode::empty_value);
  SpaceNode anonymous_parent = site;
  anonymous_parent.id = SpaceNodeId{};
  expect_status(validate_containment(anonymous_parent, rack), ErrorCode::empty_value);
}

// ---------------------------------------------------------------------------
// Node equality
// ---------------------------------------------------------------------------

void node_equality() {
  SC_CASE("SpaceNode equality is reflexive and field-sensitive");
  const SpaceNode base = good_node(SpaceNodeKind::rack);
  const SpaceNode twin = good_node(SpaceNodeKind::rack);
  SC_CHECK(base == base);
  SC_CHECK(base == twin);
  SC_CHECK(!(base != twin));

  // A different identity alone is enough to tell two nodes apart.
  SpaceNode probe = base;
  probe.id = *SpaceNodeId::parse("rack-99");
  SC_CHECK(probe != base);
  SC_CHECK(!(probe == base));

  probe = base;
  probe.generation = EntityGeneration{2};
  SC_CHECK(probe != base);
  probe = base;
  probe.kind = SpaceNodeKind::hall;
  SC_CHECK(probe != base);
  probe = base;
  probe.spatial_class = SpatialClass::floor;
  SC_CHECK(probe != base);
  probe = base;
  probe.lifecycle = NodeLifecycle::retired;
  SC_CHECK(probe != base);
  probe = base;
  probe.label = *DisplayLabel::parse("another label");
  SC_CHECK(probe != base);
  probe = base;
  probe.note = *Note::parse("a note");
  SC_CHECK(probe != base);
  probe = base;
  probe.parent = *SpaceNodeId::parse("row-2");
  SC_CHECK(probe != base);
  probe = base;
  probe.depth = 5;
  SC_CHECK(probe != base);

  // Placement, field by field.
  probe = base;
  probe.placement.has_base_rect = false;
  SC_CHECK(probe != base);
  probe = base;
  probe.placement.base_rect = PlanarRect::make(0, 0, sc_fixture::kRackWidth + 1, sc_fixture::kRackDepth);
  SC_CHECK(probe != base);
  probe = base;
  probe.placement.has_u_span = true;
  SC_CHECK(probe != base);
  probe = base;
  probe.placement.u_span = RackUnitInterval::make(1, 5);
  SC_CHECK(probe != base);

  // Envelopes.
  probe = base;
  probe.own_planar.declared_area = SquareMillimeters{1000};
  SC_CHECK(probe != base);
  probe = base;
  probe.own_planar.rects = *RectSet::build({PlanarRect::make(0, 0, 10, 10)});
  SC_CHECK(probe != base);
  probe = base;
  probe.own_rack.height = RackUnits{sc_fixture::kRackHeight + 1};
  SC_CHECK(probe != base);

  // References and evidence.
  probe = base;
  probe.location = LocationRef::of("loc-1").value();
  SC_CHECK(probe != base);
  probe = base;
  probe.facility_node = FacilityNodeRef::of("fn-1").value();
  SC_CHECK(probe != base);
  probe = base;
  probe.rack = RackRef::of("rack:a01").value();
  SC_CHECK(probe != base);
  probe = base;
  probe.assets = *AssetRefSet::build({AssetRef::of("asset-1").value()});
  SC_CHECK(probe != base);
  probe = base;
  probe.policies = *PolicyRefSet::build({PolicyRef::of("pol-1").value()});
  SC_CHECK(probe != base);
  probe = base;
  probe.evidence = one_evidence();
  SC_CHECK(probe != base);
  SC_CHECK(!probe.evidence.empty());

  // Lineage.
  probe = base;
  probe.replaced_by = *SpaceNodeId::parse("rack-99");
  SC_CHECK(probe != base);
  probe = base;
  probe.replaces = *SpaceNodeId::parse("rack-00");
  SC_CHECK(probe != base);

  // Two nodes that differ only in identity text are not equal, and two copies
  // of the same value are.
  SpaceNode first = base;
  SpaceNode second = base;
  SC_CHECK(first == second);
  second.id = *SpaceNodeId::parse("rack-02");
  SC_CHECK(first != second);
}

}  // namespace

int main() {
  kind_containment_table();
  canonical_depths_and_classes();
  lifecycle_truth_table();
  lifecycle_transition_table();
  good_nodes_validate();
  node_shape_refusals();
  containment_rules();
  node_equality();
  return ::sc_test::summary("model_test");
}
