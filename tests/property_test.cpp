// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the property test.
//
// Proves six claims against independent reference implementations written from
// scratch in this file, over fixed-seed pseudo-random inputs:
//
//   * the union-area kernel equals a brute-force integer-grid union;
//   * the containment ledger reconstructs at every plane owner, because a
//     nested plane's declared area excludes the sub-planes it contains, and the
//     rollup is then a plain sum over plane owners with no double counting;
//   * a node mutation either advances the revision by exactly one or leaves the
//     snapshot digest byte-identical;
//   * the ledger's usable and available figures equal an independent
//     reconstruction from the records;
//   * free rack-unit space, first fit and fragmentation agree with their
//     definitions; and
//   * two builds of the same model produce identical canonical bytes.
//
// No timeouts anywhere. A hang is a defect to diagnose, never a test to abort.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "dccp/space_capacity/capacity.hpp"
#include "dccp/space_capacity/claim.hpp"
#include "dccp/space_capacity/digest.hpp"
#include "dccp/space_capacity/limits.hpp"
#include "dccp/space_capacity/model.hpp"
#include "dccp/space_capacity/query.hpp"
#include "dccp/space_capacity/region.hpp"
#include "dccp/space_capacity/registry.hpp"
#include "dccp/space_capacity/requests.hpp"
#include "dccp/space_capacity/snapshot.hpp"
#include "dccp/space_capacity/units.hpp"

#include "test_support.hpp"

namespace sc = dccp::space_capacity;

namespace {

using sc::Millimeters;
using sc::PlanarRect;
using sc::RackUnitInterval;
using sc::SpaceNode;
using sc::SpaceNodeId;
using sc::SpaceNodeKind;
using sc::SpatialClass;
using sc::SquareMillimeters;

// ---------------------------------------------------------------------------
// Small shared helpers.
// ---------------------------------------------------------------------------

// Half-open rectangle intersection, written here rather than borrowed.
bool rect_overlaps_rect(const PlanarRect& a, const PlanarRect& b) {
  if (a.width.value() <= 0 || a.height.value() <= 0) return false;
  if (b.width.value() <= 0 || b.height.value() <= 0) return false;
  return a.x.value() < b.x.value() + b.width.value() &&
         b.x.value() < a.x.value() + a.width.value() &&
         a.y.value() < b.y.value() + b.height.value() &&
         b.y.value() < a.y.value() + a.height.value();
}

void append_rects(const sc::RectSet& source, std::vector<PlanarRect>& destination) {
  destination.insert(destination.end(), source.rects().begin(), source.rects().end());
}

std::int64_t saturating_subtract(std::int64_t a, std::int64_t b) { return b >= a ? 0 : a - b; }

std::string identity_text(const char* prefix, int a, int b) {
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "%s-%d-%d", prefix, a, b);
  return std::string(buffer);
}

SpaceNodeId node_id(const char* prefix, int a, int b) {
  const sc::Result<SpaceNodeId> parsed = SpaceNodeId::parse(identity_text(prefix, a, b));
  if (!parsed) {
    std::fprintf(stderr, "FATAL: could not build a node identity\n");
    std::abort();
  }
  return parsed.value();
}

sc::OccupancyClaimId claim_id(const char* prefix, int a, int b) {
  const sc::Result<sc::OccupancyClaimId> parsed =
      sc::OccupancyClaimId::parse(identity_text(prefix, a, b));
  if (!parsed) {
    std::fprintf(stderr, "FATAL: could not build a claim identity\n");
    std::abort();
  }
  return parsed.value();
}

sc::ExclusionRegionId exclusion_id(const char* prefix, int a, int b) {
  const sc::Result<sc::ExclusionRegionId> parsed =
      sc::ExclusionRegionId::parse(identity_text(prefix, a, b));
  if (!parsed) {
    std::fprintf(stderr, "FATAL: could not build an exclusion identity\n");
    std::abort();
  }
  return parsed.value();
}

sc::ExpansionZoneId zone_id(const char* prefix, int a, int b) {
  const sc::Result<sc::ExpansionZoneId> parsed =
      sc::ExpansionZoneId::parse(identity_text(prefix, a, b));
  if (!parsed) {
    std::fprintf(stderr, "FATAL: could not build an expansion zone identity\n");
    std::abort();
  }
  return parsed.value();
}

SpaceNode make_node(const SpaceNodeId& id, SpaceNodeKind kind, SpatialClass spatial_class,
                    const SpaceNodeId& parent, std::uint32_t depth) {
  SpaceNode node;
  node.id = id;
  node.generation = sc::EntityGeneration{1};
  node.kind = kind;
  node.spatial_class = spatial_class;
  node.lifecycle = sc::NodeLifecycle::available;
  node.parent = parent;
  node.depth = depth;
  return node;
}

std::optional<sc::SpaceCapacityRegistry> open_registry(const std::string& store) {
  const sc::Result<sc::StoreId> id = sc::StoreId::parse(store);
  if (!id) {
    std::fprintf(stderr, "FATAL: store identity %s was refused\n", store.c_str());
    std::abort();
  }
  sc::Result<sc::SpaceCapacityRegistry> opened =
      sc::SpaceCapacityRegistry::create_in_memory(id.value());
  if (!opened) {
    std::fprintf(stderr, "FATAL: could not open an in-memory registry: %s\n",
                 opened.error().to_string().c_str());
    std::abort();
  }
  return std::optional<sc::SpaceCapacityRegistry>(std::move(opened).value());
}

// ---------------------------------------------------------------------------
// 1. Union area, against a brute-force integer grid.
// ---------------------------------------------------------------------------
//
// Every generated coordinate is a multiple of 10 mm, so the compressed grid
// induced by the rectangle edges is exact: a cell is either wholly covered or
// wholly free, and the sum of the covered cells is the measure of the union by
// definition. The reference never calls the kernel it is checking.
std::int64_t reference_union_area(const std::vector<PlanarRect>& rects) {
  std::vector<std::int64_t> xs;
  std::vector<std::int64_t> ys;
  xs.reserve(rects.size() * 2);
  ys.reserve(rects.size() * 2);
  for (const PlanarRect& rect : rects) {
    if (rect.width.value() <= 0 || rect.height.value() <= 0) continue;
    xs.push_back(rect.x.value());
    xs.push_back(rect.x.value() + rect.width.value());
    ys.push_back(rect.y.value());
    ys.push_back(rect.y.value() + rect.height.value());
  }
  if (xs.empty()) return 0;
  std::sort(xs.begin(), xs.end());
  xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

  std::int64_t total = 0;
  for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
    for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
      const std::int64_t left = xs[i];
      const std::int64_t right = xs[i + 1];
      const std::int64_t top = ys[j];
      const std::int64_t bottom = ys[j + 1];
      if (right <= left || bottom <= top) continue;
      bool covered = false;
      for (const PlanarRect& rect : rects) {
        if (rect.width.value() <= 0 || rect.height.value() <= 0) continue;
        if (rect.x.value() <= left && left < rect.x.value() + rect.width.value() &&
            rect.y.value() <= top && top < rect.y.value() + rect.height.value()) {
          covered = true;
          break;
        }
      }
      if (covered) total += (right - left) * (bottom - top);
    }
  }
  return total;
}

constexpr std::int64_t kStep = 10;
constexpr std::int64_t kUnionLimit = 2000;

PlanarRect random_union_rect(sc_test::Rng& rng) {
  const std::int64_t sizes[4] = {200, 400, 600, 1000};
  const std::int64_t width = sizes[rng.bounded(4)];
  const std::int64_t height = sizes[rng.bounded(4)];
  const std::int64_t x = (rng.between(0, kUnionLimit - width) / kStep) * kStep;
  const std::int64_t y = (rng.between(0, kUnionLimit - height) / kStep) * kStep;
  return PlanarRect::make(x, y, width, height);
}

// Mixes disjoint, duplicate, contained and overlapping rectangles on purpose.
void fill_rectangle_set(sc_test::Rng& rng, std::vector<PlanarRect>& rects) {
  const std::size_t count = static_cast<std::size_t>(rng.between(1, 12));
  rects.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint64_t mode = rects.empty() ? 0u : rng.bounded(5);
    if (mode == 0) {
      rects.push_back(random_union_rect(rng));
    } else if (mode == 1) {
      // A disjoint rectangle: one cell of a 4 x 4 grid of 500 mm cells.
      const std::int64_t cx = static_cast<std::int64_t>(rng.bounded(4)) * 500;
      const std::int64_t cy = static_cast<std::int64_t>(rng.bounded(4)) * 500;
      rects.push_back(PlanarRect::make(cx, cy, 500, 500));
    } else if (mode == 2) {
      // An exact duplicate of an earlier rectangle.
      rects.push_back(rects[rng.bounded(static_cast<std::uint64_t>(rects.size()))]);
    } else if (mode == 3) {
      // A rectangle strictly inside an earlier one.
      const PlanarRect outer = rects[rng.bounded(static_cast<std::uint64_t>(rects.size()))];
      const std::int64_t inner_width = (outer.width.value() / 2 / kStep) * kStep;
      const std::int64_t inner_height = (outer.height.value() / 2 / kStep) * kStep;
      rects.push_back(PlanarRect::make(outer.x.value() + inner_width / 2,
                                       outer.y.value() + inner_height / 2, inner_width,
                                       inner_height));
    } else {
      // A rectangle that deliberately overlaps an earlier one.
      const PlanarRect other = rects[rng.bounded(static_cast<std::uint64_t>(rects.size()))];
      const std::int64_t shift = static_cast<std::int64_t>(rng.bounded(4)) * kStep + kStep;
      const std::int64_t max_x = kUnionLimit - other.width.value();
      const std::int64_t max_y = kUnionLimit - other.height.value();
      const std::int64_t x = (std::min(other.x.value() + shift, max_x) / kStep) * kStep;
      const std::int64_t y = (std::min(other.y.value() + shift, max_y) / kStep) * kStep;
      rects.push_back(PlanarRect::make(x, y, other.width.value(), other.height.value()));
    }
  }
}

void test_union_area(int cases) {
  SC_CASE("union_area");
  sc_test::Rng rng(0x5CA9E001ull);
  int overflow_reports = 0;
  for (int i = 0; i < cases; ++i) {
    std::vector<PlanarRect> rects;
    fill_rectangle_set(rng, rects);
    const sc::Checked<SquareMillimeters> measured = sc::rect_union_area(rects);
    if (measured.overflowed) ++overflow_reports;
    SC_CHECK(!measured.overflowed);
    const std::int64_t expected = reference_union_area(rects);
    SC_CHECK_EQ(measured.value.value(), expected);

    const sc::Result<sc::RectSet> set = sc::RectSet::build(rects);
    if (set) {
      const sc::Checked<SquareMillimeters> via_set = sc::rect_set_union_area(set.value());
      SC_CHECK(!via_set.overflowed);
      SC_CHECK_EQ(via_set.value.value(), expected);
    }
  }
  SC_CHECK_EQ(overflow_reports, 0);
  std::fprintf(stdout, "union_area: %d rectangle sets checked against the grid reference\n",
               cases);
}

// ---------------------------------------------------------------------------
// A public-API mutation driver. Request keys are empty throughout, so no
// request is ever replayed and every call is a fresh attempt.
// ---------------------------------------------------------------------------
class Planner final {
 public:
  Planner(sc::SpaceCapacityRegistry& registry, std::string name)
      : registry_(&registry), name_(std::move(name)) {}

  bool create(const SpaceNode& node) {
    sc::CreateNodeRequest request;
    request.node = node;
    return note(registry_->apply(request), node.id.str());
  }

  bool set_planar(const SpaceNodeId& node, std::int64_t width, std::int64_t depth) {
    sc::SetNodeEnvelopeRequest request;
    request.node = node;
    sc::PlanarEnvelope envelope;
    envelope.declared_area = SquareMillimeters{width * depth};
    const sc::Result<sc::RectSet> rects =
        sc::RectSet::build({PlanarRect::make(0, 0, width, depth)});
    if (!rects) return false;
    envelope.rects = rects.value();
    request.planar = envelope;
    return note(registry_->apply(request), node.str());
  }

  bool set_planar_area(const SpaceNodeId& node, std::int64_t area) {
    sc::SetNodeEnvelopeRequest request;
    request.node = node;
    sc::PlanarEnvelope envelope;
    envelope.declared_area = SquareMillimeters{area};
    request.planar = envelope;
    return note(registry_->apply(request), node.str());
  }

  bool set_rack_envelope(const SpaceNodeId& node, std::int32_t height) {
    sc::SetNodeEnvelopeRequest request;
    request.node = node;
    sc::RackEnvelope envelope;
    envelope.height = sc::RackUnits{height};
    request.rack = envelope;
    return note(registry_->apply(request), node.str());
  }

  bool set_placement(const SpaceNodeId& node, PlanarRect rect) {
    sc::SetNodePlacementRequest request;
    request.node = node;
    request.placement.base_rect = rect;
    request.placement.has_base_rect = true;
    return note(registry_->apply(request), node.str());
  }

  bool clear_placement(const SpaceNodeId& node) {
    sc::SetNodePlacementRequest request;
    request.node = node;
    request.placement = sc::NodePlacement{};
    return note(registry_->apply(request), node.str());
  }

  bool create_claim(const sc::OccupancyClaimId& id, const SpaceNodeId& node, PlanarRect rect,
                    sc::ClaimState state) {
    sc::CreateClaimRequest request;
    request.claim.id = id;
    request.claim.generation = sc::EntityGeneration{1};
    request.claim.node = node;
    request.claim.state = state;
    request.claim.occupant = sc::OccupantKind::infrastructure;
    request.claim.scope.kind = sc::FootprintScopeKind::planar;
    const sc::Result<sc::RectSet> rects = sc::RectSet::build({rect});
    if (!rects) return false;
    request.claim.scope.rects = rects.value();
    return note(registry_->apply(request), id.str());
  }

  bool create_reservation(const sc::OccupancyClaimId& id, const SpaceNodeId& node,
                          RackUnitInterval span) {
    sc::CreateReservationRequest request;
    request.reservation.id = id;
    request.reservation.generation = sc::EntityGeneration{1};
    request.reservation.node = node;
    request.reservation.state = sc::ReservationState::held;
    const sc::Result<sc::ReservationRef> authority =
        sc::ReservationRef::make(identity_text("res", 1, 1), 1, sc::UpstreamState::active);
    if (!authority) return false;
    request.reservation.reservation = authority.value();
    request.reservation.scope.kind = sc::FootprintScopeKind::rack_units;
    const sc::Result<sc::IntervalSet> units = sc::IntervalSet::build({span});
    if (!units) return false;
    request.reservation.scope.units = units.value();
    return note(registry_->apply(request), id.str());
  }

  bool create_exclusion(const sc::ExclusionRegionId& id, const SpaceNodeId& node, PlanarRect rect) {
    sc::CreateExclusionRequest request;
    request.region.id = id;
    request.region.generation = sc::EntityGeneration{1};
    request.region.node = node;
    request.region.reason = sc::ExclusionReason::thermal;
    request.region.state = sc::ExclusionState::active;
    request.region.blocks = sc::default_mask_for_reason(sc::ExclusionReason::thermal);
    request.region.scope.kind = sc::FootprintScopeKind::planar;
    const sc::Result<sc::RectSet> rects = sc::RectSet::build({rect});
    if (!rects) return false;
    request.region.scope.rects = rects.value();
    return note(registry_->apply(request), id.str());
  }

  bool create_zone(const sc::ExpansionZoneId& id, const SpaceNodeId& node, PlanarRect rect) {
    sc::CreateExpansionZoneRequest request;
    request.zone.id = id;
    request.zone.generation = sc::EntityGeneration{1};
    request.zone.node = node;
    request.zone.state = sc::ExpansionState::funded;
    request.zone.scope.kind = sc::FootprintScopeKind::planar;
    const sc::Result<sc::RectSet> rects = sc::RectSet::build({rect});
    if (!rects) return false;
    request.zone.scope.rects = rects.value();
    return note(registry_->apply(request), id.str());
  }

  const std::string& first_refusal() const { return refusal_; }

 private:
  bool note(const sc::Result<sc::MutationOutcome>& outcome, const std::string& subject) {
    if (outcome) return true;
    if (refusal_.empty()) refusal_ = subject + ": " + outcome.error().to_string();
    return false;
  }

  sc::SpaceCapacityRegistry* registry_;
  std::string name_;
  std::string refusal_{};
};

// ---------------------------------------------------------------------------
// Model plans: pure data, so one plan can be replayed byte for byte.
// ---------------------------------------------------------------------------

constexpr std::int64_t kCell = 1000;
constexpr std::int32_t kRackHeight = 42;

struct Cell final {
  std::int32_t col = 0;
  std::int32_t row = 0;
};

struct ModelPlan final {
  std::string name;
  std::string store;
  std::int32_t grid_cols = 4;
  std::int32_t grid_rows = 4;
  std::int32_t rack_count = 0;
  std::int32_t claim_count = 0;
  std::int32_t exclusion_count = 0;
  std::int32_t zone_count = 0;
  bool has_annex = false;
  Cell annex{};
  std::vector<Cell> racks;
  std::vector<Cell> claims;
  std::vector<Cell> exclusions;
  std::vector<Cell> zones;
};

class Grid final {
 public:
  Grid(std::int32_t cols, std::int32_t rows)
      : cols_(cols), rows_(rows), used_(cell_count(), 0) {}

  bool free(const Cell& cell) const {
    if (cell.col < 0 || cell.row < 0 || cell.col >= cols_ || cell.row >= rows_) return false;
    return used_[index(cell)] == 0;
  }
  void take(const Cell& cell) { used_[index(cell)] = 1; }

  std::optional<Cell> take_random_free(sc_test::Rng& rng) {
    for (int attempt = 0; attempt < 64; ++attempt) {
      const Cell cell{static_cast<std::int32_t>(rng.bounded(static_cast<std::uint64_t>(cols_))),
                      static_cast<std::int32_t>(rng.bounded(static_cast<std::uint64_t>(rows_)))};
      if (free(cell)) {
        take(cell);
        return cell;
      }
    }
    for (std::int32_t row = 0; row < rows_; ++row) {
      for (std::int32_t col = 0; col < cols_; ++col) {
        const Cell cell{col, row};
        if (free(cell)) {
          take(cell);
          return cell;
        }
      }
    }
    return std::nullopt;
  }

 private:
  std::size_t cell_count() const {
    return static_cast<std::size_t>(cols_) * static_cast<std::size_t>(rows_);
  }
  std::size_t index(const Cell& cell) const {
    return static_cast<std::size_t>(cell.row) * static_cast<std::size_t>(cols_) +
           static_cast<std::size_t>(cell.col);
  }

  std::int32_t cols_;
  std::int32_t rows_;
  std::vector<unsigned char> used_;
};

PlanarRect cell_rect(const Cell& cell) {
  return PlanarRect::make(static_cast<std::int64_t>(cell.col) * kCell,
                          static_cast<std::int64_t>(cell.row) * kCell, kCell, kCell);
}

// Two stacked cells, so a subdivided plane is not square and its declared area
// can only match its placement if both are the true rectangle area.
PlanarRect annex_rect(const Cell& cell) {
  return PlanarRect::make(static_cast<std::int64_t>(cell.col) * kCell,
                          static_cast<std::int64_t>(cell.row) * kCell, kCell, 2 * kCell);
}

ModelPlan make_model_plan(int seed) {
  sc_test::Rng rng(static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 11u);
  ModelPlan plan;
  plan.name = identity_text("pm", seed, 0);
  plan.store = identity_text("st", seed, 0);
  plan.grid_cols = static_cast<std::int32_t>(rng.between(4, 6));
  plan.grid_rows = static_cast<std::int32_t>(rng.between(4, 6));
  plan.rack_count = static_cast<std::int32_t>(rng.between(0, 8));
  plan.claim_count = static_cast<std::int32_t>(rng.between(0, 5));
  plan.exclusion_count = static_cast<std::int32_t>(rng.between(0, 3));
  plan.zone_count = static_cast<std::int32_t>(rng.between(0, 3));
  plan.has_annex = rng.bounded(2) != 0;

  Grid grid(plan.grid_cols, plan.grid_rows);
  if (plan.has_annex) {
    bool placed = false;
    for (std::int32_t col = 0; col < plan.grid_cols && !placed; ++col) {
      for (std::int32_t row = 0; row + 1 < plan.grid_rows && !placed; ++row) {
        const Cell top{col, row};
        const Cell bottom{col, row + 1};
        if (grid.free(top) && grid.free(bottom)) {
          grid.take(top);
          grid.take(bottom);
          plan.annex = top;
          placed = true;
        }
      }
    }
    plan.has_annex = placed;
  }
  for (std::int32_t i = 0; i < plan.rack_count; ++i) {
    const std::optional<Cell> cell = grid.take_random_free(rng);
    if (!cell.has_value()) break;
    plan.racks.push_back(*cell);
  }
  plan.rack_count = static_cast<std::int32_t>(plan.racks.size());
  for (std::int32_t i = 0; i < plan.zone_count; ++i) {
    const std::optional<Cell> cell = grid.take_random_free(rng);
    if (!cell.has_value()) break;
    plan.zones.push_back(*cell);
  }
  plan.zone_count = static_cast<std::int32_t>(plan.zones.size());
  for (std::int32_t i = 0; i < plan.exclusion_count; ++i) {
    // An exclusion is refused where it covers a consuming record, so it goes on
    // a cell no rack and no zone holds.
    const std::optional<Cell> cell = grid.take_random_free(rng);
    if (!cell.has_value()) break;
    plan.exclusions.push_back(*cell);
  }
  plan.exclusion_count = static_cast<std::int32_t>(plan.exclusions.size());
  for (std::int32_t i = 0; i < plan.claim_count; ++i) {
    // Claims go on free cells, so no claim lands inside an exclusion and the
    // strict ledger identity is expected to hold for every accepted model.
    const std::optional<Cell> cell = grid.take_random_free(rng);
    if (!cell.has_value()) break;
    plan.claims.push_back(*cell);
  }
  plan.claim_count = static_cast<std::int32_t>(plan.claims.size());
  return plan;
}

// Builds a plan entirely through the public mutation API.
bool build_model(const ModelPlan& plan, Planner& planner) {
  const std::int64_t hall_width = static_cast<std::int64_t>(plan.grid_cols) * kCell;
  const std::int64_t hall_depth = static_cast<std::int64_t>(plan.grid_rows) * kCell;

  const SpaceNodeId site = node_id("site", 0, 0);
  const SpaceNodeId hall = node_id("hall", 0, 0);
  const SpaceNodeId annex = node_id("annex", 0, 0);

  if (!planner.create(
          make_node(site, SpaceNodeKind::site, SpatialClass::outdoor, SpaceNodeId{}, 0))) {
    return false;
  }
  if (!planner.create(make_node(hall, SpaceNodeKind::hall, SpatialClass::floor, site, 1))) {
    return false;
  }
  if (!planner.set_planar(hall, hall_width, hall_depth)) return false;

  if (plan.has_annex) {
    // The only plane-declaring node a hall may contain is a row, so the
    // sub-plane is a row that declares the area it occupies.
    if (!planner.create(make_node(annex, SpaceNodeKind::row, SpatialClass::aisle, hall, 2))) {
      return false;
    }
    const PlanarRect rect = annex_rect(plan.annex);
    if (!planner.set_placement(annex, rect)) return false;
    if (!planner.set_planar_area(annex, rect.width.value() * rect.height.value())) return false;
  }

  for (std::int32_t i = 0; i < plan.rack_count; ++i) {
    const SpaceNodeId rack = node_id("rack", i, 0);
    if (!planner.create(make_node(rack, SpaceNodeKind::rack, SpatialClass::rack, hall, 2))) {
      return false;
    }
    if (!planner.set_rack_envelope(rack, kRackHeight)) return false;
    if (!planner.set_placement(rack, cell_rect(plan.racks[static_cast<std::size_t>(i)]))) {
      return false;
    }
  }
  for (std::int32_t i = 0; i < plan.exclusion_count; ++i) {
    if (!planner.create_exclusion(exclusion_id("x", static_cast<int>(i), 0), hall,
                                  cell_rect(plan.exclusions[static_cast<std::size_t>(i)]))) {
      return false;
    }
  }
  for (std::int32_t i = 0; i < plan.zone_count; ++i) {
    if (!planner.create_zone(zone_id("z", static_cast<int>(i), 0), hall,
                             cell_rect(plan.zones[static_cast<std::size_t>(i)]))) {
      return false;
    }
  }
  for (std::int32_t i = 0; i < plan.claim_count; ++i) {
    if (!planner.create_claim(claim_id("c", static_cast<int>(i), 0), hall,
                              cell_rect(plan.claims[static_cast<std::size_t>(i)]),
                              sc::ClaimState::committed)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Ledger reconstruction from the records themselves.
// ---------------------------------------------------------------------------

struct PlaneRaw final {
  // The union of the exclusions and the enforceable clearance bands: what the
  // library calls `excluded`, before the saturation at the declared area.
  std::int64_t excluded_union = 0;
  // The union of everything a placement must avoid, excluding the sub-plane
  // placements: exclusions, clearance bands, structural placements, committed
  // claims, held reservations and earmarking zones.
  std::int64_t blocked_union = 0;
  // The union of the structural placements alone. Under disjointness this is
  // the structural measure the ledger reports.
  std::int64_t structural_union = 0;
  // The union of the placements of the plane-owning nodes that subdivide this
  // plane. Their area has already left `declared`.
  std::int64_t subplane_union = 0;
  // True when the authority records on this plane are pairwise disjoint, which
  // is what makes the strict sum identity the right assertion.
  bool records_disjoint = true;
};

std::int64_t union_area_of(const std::vector<PlanarRect>& rects) {
  const sc::Checked<SquareMillimeters> measured = sc::rect_union_area(rects);
  if (!measured.ok()) {
    std::fprintf(stderr, "FATAL: the union of %zu rectangles overflowed\n", rects.size());
    std::abort();
  }
  return measured.value.value();
}

bool any_overlap(const std::vector<PlanarRect>& left, const std::vector<PlanarRect>& right) {
  for (const PlanarRect& a : left) {
    for (const PlanarRect& b : right) {
      if (rect_overlaps_rect(a, b)) return true;
    }
  }
  return false;
}

PlaneRaw plane_raw(const sc::Snapshot& snapshot, const SpaceNodeId& plane) {
  PlaneRaw raw;
  std::vector<PlanarRect> exclusions;
  std::vector<PlanarRect> clearances;
  std::vector<PlanarRect> structural;
  std::vector<PlanarRect> subplanes;
  std::vector<PlanarRect> claims;
  std::vector<PlanarRect> held;
  std::vector<PlanarRect> earmarked;

  for (const SpaceNode& node : snapshot.nodes()) {
    if (!node.placement.has_base_rect) continue;
    // A plane owner's own placement belongs to the plane that contains it, not
    // to the plane it declares, so it is not a placement "on" this plane. The
    // library attributes exactly the same way.
    const SpaceNodeId owner = snapshot.plane_owner_of(node.id);
    if (owner != plane || owner == node.id) continue;
    if (node.own_planar.is_declared()) {
      subplanes.push_back(node.placement.base_rect);
    } else {
      structural.push_back(node.placement.base_rect);
    }
  }
  for (const sc::OccupancyClaim& claim : snapshot.claims()) {
    if (!claim.consumes()) continue;
    if (snapshot.plane_owner_of(claim.node) != plane) continue;
    append_rects(claim.scope.rects, claims);
  }
  for (const sc::FootprintReservation& reservation : snapshot.reservations()) {
    if (!sc::reservation_state_holds(reservation.state)) continue;
    if (snapshot.plane_owner_of(reservation.node) != plane) continue;
    append_rects(reservation.scope.rects, held);
  }
  for (const sc::ExclusionRegion& region : snapshot.exclusions()) {
    if (!region.blocks_now() || region.blocks.empty()) continue;
    if (snapshot.plane_owner_of(region.node) != plane) continue;
    append_rects(region.scope.rects, exclusions);
  }
  for (const sc::ClearanceConstraint& clearance : snapshot.clearances()) {
    if (!clearance.enforceable || !clearance.has_band) continue;
    if (snapshot.plane_owner_of(clearance.node) != plane) continue;
    clearances.push_back(clearance.band);
  }
  for (const sc::ExpansionZone& zone : snapshot.expansion_zones()) {
    if (!zone.earmarks()) continue;
    if (snapshot.plane_owner_of(zone.node) != plane) continue;
    append_rects(zone.scope.rects, earmarked);
  }

  std::vector<PlanarRect> excluded_input = exclusions;
  excluded_input.insert(excluded_input.end(), clearances.begin(), clearances.end());
  raw.excluded_union = union_area_of(excluded_input);

  std::vector<PlanarRect> blockers = exclusions;
  blockers.insert(blockers.end(), clearances.begin(), clearances.end());
  blockers.insert(blockers.end(), structural.begin(), structural.end());
  blockers.insert(blockers.end(), claims.begin(), claims.end());
  blockers.insert(blockers.end(), held.begin(), held.end());
  blockers.insert(blockers.end(), earmarked.begin(), earmarked.end());
  raw.blocked_union = union_area_of(blockers);
  raw.structural_union = union_area_of(structural);
  raw.subplane_union = union_area_of(subplanes);

  raw.records_disjoint = !any_overlap(exclusions, clearances) &&
                         !any_overlap(exclusions, structural) &&
                         !any_overlap(exclusions, claims) &&
                         !any_overlap(exclusions, earmarked) &&
                         !any_overlap(clearances, structural) &&
                         !any_overlap(clearances, claims) &&
                         !any_overlap(clearances, subplanes) &&
                         !any_overlap(structural, claims) &&
                         !any_overlap(structural, earmarked) &&
                         !any_overlap(structural, subplanes) &&
                         !any_overlap(claims, earmarked) &&
                         !any_overlap(held, claims) &&
                         !any_overlap(held, structural) &&
                         !any_overlap(held, earmarked) &&
                         !any_overlap(held, exclusions) &&
                         !any_overlap(held, clearances);
  return raw;
}

// The ledger claims, each recomputed here from the records rather than read
// back from the ledger:
//   usable    == declared - excluded
//   available == declared - min(union(blockers) + subplanes, declared)
//   declared  == excluded + structural + claimed + held + earmarked
//                + subplanes + available
// The last one holds exactly when no two authority records overlap; when two
// do, their union is smaller than their sum and only the weaker claims survive.
void check_plane_ledger(const sc::Snapshot& snapshot, const SpaceNodeId& plane, bool strict) {
  const PlaneRaw raw = plane_raw(snapshot, plane);
  const sc::NodeCapacity capacity = snapshot.capacity_of(plane, sc::FitContext{});
  const sc::AreaLedger& area = capacity.area;
  const std::int64_t declared = area.declared.value();

  // A plane that is out of service or still planned is entirely unavailable by
  // definition: everything it declares counts as excluded, and nothing is
  // reported as available or over-committed.
  const bool in_service = sc::lifecycle_is_usable_now(capacity.lifecycle);

  SC_CHECK(declared >= 0);
  SC_CHECK(area.excluded.value() <= declared);
  SC_CHECK(area.usable.value() <= declared);
  SC_CHECK(area.available.value() <= area.usable.value());
  SC_CHECK(area.available.value() >= 0);

  const std::int64_t consumed = raw.blocked_union + raw.subplane_union;
  if (in_service) {
    const std::int64_t expected_excluded = std::min(raw.excluded_union, declared);
    SC_CHECK_EQ(area.excluded.value(), expected_excluded);
    SC_CHECK_EQ(area.usable.value(), saturating_subtract(declared, expected_excluded));
    SC_CHECK_EQ(area.available.value(), saturating_subtract(declared, consumed));
    SC_CHECK_EQ(area.over_committed, consumed > declared);
  } else {
    SC_CHECK_EQ(area.excluded.value(), declared);
    SC_CHECK_EQ(area.usable.value(), 0);
    SC_CHECK_EQ(area.available.value(), 0);
    SC_CHECK(!area.over_committed);
  }

  const std::int64_t structural = area.structural.value();
  const std::int64_t claimed = area.claimed.value();
  const std::int64_t held = area.held.value();
  const std::int64_t earmarked = area.earmarked.value();
  const std::int64_t available = area.available.value();

  if (strict) {
    if (raw.records_disjoint) {
      // Union equals sum family by family, so every family's measure is its
      // union and the strict identity holds. The exclusion family is included:
      // it is disjoint from the consuming records, and `blocked` is the union of
      // all of them together.
      SC_CHECK_EQ(area.excluded.value(), raw.excluded_union);
      SC_CHECK_EQ(area.excluded.value() + structural + claimed + held + earmarked,
                  raw.blocked_union);
      SC_CHECK_EQ(structural, raw.structural_union);
      SC_CHECK_EQ(area.excluded.value() + structural + claimed + held + earmarked +
                      raw.subplane_union + available,
                  declared);
    } else {
      // Two authority records overlap, so a union is strictly smaller than a
      // sum and only the weaker claims are asserted.
      SC_CHECK(available <= saturating_subtract(declared, area.excluded.value()));
    }
  }
}

// ---------------------------------------------------------------------------
// 2. Containment accounting.
// ---------------------------------------------------------------------------

struct ModelTally final {
  int accepted = 0;
  int refused = 0;
  int with_subplane = 0;
  int exact_identity = 0;
  int weaker_identity = 0;
};

ModelTally test_containment_accounting(int cases) {
  SC_CASE("containment_accounting");
  ModelTally tally;
  for (int i = 0; i < cases; ++i) {
    const ModelPlan plan = make_model_plan(1000 + i);
    std::optional<sc::SpaceCapacityRegistry> registry = open_registry(plan.store);
    Planner planner(*registry, plan.name);
    if (!build_model(plan, planner)) {
      ++tally.refused;
      if (tally.refused <= 3) {
        std::fprintf(stdout, "containment: model %s refused: %s\n", plan.name.c_str(),
                     planner.first_refusal().c_str());
      }
      continue;
    }
    ++tally.accepted;
    if (plan.has_annex) ++tally.with_subplane;

    const sc::SnapshotPtr snapshot = registry->snapshot();
    SC_CHECK(snapshot != nullptr);
    SC_CHECK(snapshot->audit().ok());

    std::vector<SpaceNodeId> owners;
    for (const SpaceNode& node : snapshot->nodes()) {
      if (node.own_planar.is_declared()) owners.push_back(node.id);
    }
    SC_CHECK(!owners.empty());

    std::int64_t declared_total = 0;
    std::int64_t available_total = 0;
    for (const SpaceNodeId& owner : owners) {
      const sc::NodeCapacity capacity = snapshot->capacity_of(owner, sc::FitContext{});
      SC_CHECK(capacity.owns_plane);
      SC_CHECK(capacity.area.available <= capacity.area.usable);
      SC_CHECK(capacity.area.usable <= capacity.area.declared);
      SC_CHECK_EQ(capacity.area.usable.value(),
                  saturating_subtract(capacity.area.declared.value(),
                                      capacity.area.excluded.value()));
      declared_total += capacity.area.declared.value();
      available_total += capacity.area.available.value();
      check_plane_ledger(*snapshot, owner, true);
      const PlaneRaw raw = plane_raw(*snapshot, owner);
      if (raw.records_disjoint) {
        ++tally.exact_identity;
      } else {
        ++tally.weaker_identity;
      }
    }

    // The rollup is a plain sum over plane owners: a nested plane is a
    // subdivision of the plane above it, so no plane is counted twice and none
    // is missed.
    const sc::CapacityRollup rollup = sc::model_rollup(*snapshot, sc::FitContext{});
    SC_CHECK_EQ(declared_total, rollup.area.declared.value());
    SC_CHECK_EQ(available_total, rollup.area.available.value());
    SC_CHECK_EQ(rollup.plane_owner_count, static_cast<std::uint64_t>(owners.size()));
    SC_CHECK(rollup.area.available.value() <= rollup.area.declared.value());
  }
  std::fprintf(stdout,
               "containment: %d models accepted, %d refused (%d with a sub-plane); "
               "%d plane ledgers disjoint, %d overlapping\n",
               tally.accepted, tally.refused, tally.with_subplane, tally.exact_identity,
               tally.weaker_identity);
  return tally;
}

// A whole-node record covers the whole declared envelope and carries no
// rectangle, so the ledger must saturate rather than measure nothing. This is
// checked directly because the rectangular reconstruction cannot see it.
void test_whole_node_scope() {
  SC_CASE("whole_node_scope");
  ModelPlan plan;
  plan.name = "wn";
  plan.store = "st-wn";
  plan.grid_cols = 4;
  plan.grid_rows = 4;
  std::optional<sc::SpaceCapacityRegistry> registry = open_registry(plan.store);
  Planner planner(*registry, plan.name);
  SC_CHECK(build_model(plan, planner));

  const SpaceNodeId hall = node_id("hall", 0, 0);
  const SpaceNodeId empty_hall = node_id("wn-empty", 0, 0);
  SC_CHECK(planner.create(
      make_node(empty_hall, SpaceNodeKind::hall, SpatialClass::floor, node_id("site", 0, 0), 1)));
  SC_CHECK(planner.set_planar(empty_hall, 4000, 4000));

  {
    sc::CreateClaimRequest request;
    request.claim.id = claim_id("wn", 1, 0);
    request.claim.generation = sc::EntityGeneration{1};
    request.claim.node = hall;
    request.claim.state = sc::ClaimState::committed;
    request.claim.occupant = sc::OccupantKind::workload;
    request.claim.scope.kind = sc::FootprintScopeKind::whole_node;
    const sc::Result<sc::MutationOutcome> outcome = registry->apply(request);
    SC_CHECK(outcome.ok());
    if (!outcome) return;
  }
  const sc::SnapshotPtr after_claim = registry->snapshot();
  const sc::NodeCapacity claimed = after_claim->capacity_of(hall, sc::FitContext{});
  SC_CHECK_EQ(claimed.area.claimed.value(), claimed.area.declared.value());
  SC_CHECK_EQ(claimed.area.available.value(), 0);
  SC_CHECK(!claimed.area.over_committed);
  SC_CHECK(after_claim->audit().ok());

  // A whole-node claim covers the entire envelope, so a second consuming record
  // on the same plane is refused rather than silently overlapping.
  {
    sc::CreateExpansionZoneRequest request;
    request.zone.id = zone_id("wn", 1, 0);
    request.zone.generation = sc::EntityGeneration{1};
    request.zone.node = hall;
    request.zone.state = sc::ExpansionState::funded;
    request.zone.scope.kind = sc::FootprintScopeKind::whole_node;
    const sc::Result<sc::MutationOutcome> outcome = registry->apply(request);
    SC_CHECK(!outcome.ok());
  }
  SC_CHECK(registry->snapshot()->digest() == after_claim->digest());

  // On a plane no consuming record holds, a whole-node zone earmarks all of it.
  {
    sc::CreateExpansionZoneRequest request;
    request.zone.id = zone_id("wn", 2, 0);
    request.zone.generation = sc::EntityGeneration{1};
    request.zone.node = empty_hall;
    request.zone.state = sc::ExpansionState::funded;
    request.zone.scope.kind = sc::FootprintScopeKind::whole_node;
    const sc::Result<sc::MutationOutcome> outcome = registry->apply(request);
    SC_CHECK(outcome.ok());
  }
  const sc::SnapshotPtr after_zone = registry->snapshot();
  const sc::NodeCapacity earmarked = after_zone->capacity_of(empty_hall, sc::FitContext{});
  SC_CHECK_EQ(earmarked.area.earmarked.value(), earmarked.area.declared.value());
  SC_CHECK_EQ(earmarked.area.available.value(), 0);
  SC_CHECK_EQ(earmarked.area.claimed.value(), 0);
  SC_CHECK(after_zone->audit().ok());
}

// ---------------------------------------------------------------------------
// 3. Move, reparent, replacement and retirement sequences.
// ---------------------------------------------------------------------------

struct MoveNode final {
  SpaceNodeId id{};
  SpaceNodeKind kind = SpaceNodeKind::none;
  SpaceNodeId parent{};
  bool has_cell = false;
  Cell cell{};
  bool has_plane = false;
  bool has_rack_envelope = false;
};

// True when `ancestor` appears on the parent chain of `descendant`.
bool is_ancestor_in(const std::vector<MoveNode>& nodes, const SpaceNodeId& ancestor,
                    const SpaceNodeId& descendant) {
  if (ancestor.empty() || ancestor == descendant) return false;
  SpaceNodeId cursor = descendant;
  for (std::uint32_t hops = 0; hops < 64; ++hops) {
    const SpaceNodeId* next = nullptr;
    for (const MoveNode& entry : nodes) {
      if (entry.id == cursor) {
        next = &entry.parent;
        break;
      }
    }
    if (next == nullptr || next->empty()) return false;
    if (*next == ancestor) return true;
    cursor = *next;
  }
  return false;
}

// Every consuming record on one plane, collected independently of the library's
// own conflict search: node placements (including the placements of nested
// plane owners), committed claims, held reservations and earmarking zones.
std::vector<PlanarRect> consuming_rects_on(const sc::Snapshot& snapshot,
                                           const SpaceNodeId& plane) {
  std::vector<PlanarRect> rects;
  for (const SpaceNode& node : snapshot.nodes()) {
    if (!node.placement.has_base_rect) continue;
    if (snapshot.plane_owner_of(node.id) != plane) continue;
    rects.push_back(node.placement.base_rect);
  }
  for (const sc::OccupancyClaim& claim : snapshot.claims()) {
    if (!claim.consumes()) continue;
    if (snapshot.plane_owner_of(claim.node) != plane) continue;
    append_rects(claim.scope.rects, rects);
  }
  for (const sc::FootprintReservation& reservation : snapshot.reservations()) {
    if (!sc::reservation_state_holds(reservation.state)) continue;
    if (snapshot.plane_owner_of(reservation.node) != plane) continue;
    append_rects(reservation.scope.rects, rects);
  }
  for (const sc::ExpansionZone& zone : snapshot.expansion_zones()) {
    if (!zone.earmarks()) continue;
    if (snapshot.plane_owner_of(zone.node) != plane) continue;
    append_rects(zone.scope.rects, rects);
  }
  return rects;
}

void check_tree_invariants(const sc::Snapshot& snapshot, const SpaceNodeId& site) {
  for (const SpaceNode& node : snapshot.nodes()) {
    if (node.parent.empty()) {
      SC_CHECK_EQ(node.depth, 0u);
      SC_CHECK(node.id.value() == site.value());
    } else {
      const SpaceNode* parent = snapshot.find_node(node.parent);
      SC_CHECK(parent != nullptr);
      if (parent != nullptr) {
        SC_CHECK_EQ(node.depth, parent->depth + 1u);
        SC_CHECK(node.depth > 0u);
      }
    }
    // No node is its own ancestor, and every chain reaches a root. The hop
    // bound is what keeps a cycle from hanging the test rather than a timeout.
    SpaceNodeId cursor = node.parent;
    bool reached_root = node.parent.empty();
    bool revisited = false;
    for (std::uint32_t hops = 0; !cursor.empty() && hops < 64; ++hops) {
      if (cursor == node.id) {
        revisited = true;
        break;
      }
      const SpaceNode* next = snapshot.find_node(cursor);
      if (next == nullptr) break;
      if (next->parent.empty()) {
        reached_root = true;
        break;
      }
      cursor = next->parent;
    }
    SC_CHECK(!revisited);
    SC_CHECK(reached_root);
  }

  // No two consuming records on the same plane overlap.
  for (const SpaceNode& node : snapshot.nodes()) {
    if (!node.own_planar.is_declared()) continue;
    const std::vector<PlanarRect> rects = consuming_rects_on(snapshot, node.id);
    for (std::size_t i = 0; i < rects.size(); ++i) {
      for (std::size_t j = i + 1; j < rects.size(); ++j) {
        SC_CHECK(!rect_overlaps_rect(rects[i], rects[j]));
      }
    }
  }

  // No two consuming records on any rack envelope cover the same rack unit.
  std::vector<RackUnitInterval> spans;
  for (const SpaceNode& node : snapshot.nodes()) {
    if (node.placement.has_u_span) spans.push_back(node.placement.u_span);
  }
  for (const sc::OccupancyClaim& claim : snapshot.claims()) {
    if (!claim.consumes()) continue;
    for (const RackUnitInterval& interval : claim.scope.units.intervals()) {
      spans.push_back(interval);
    }
  }
  for (const sc::FootprintReservation& reservation : snapshot.reservations()) {
    if (!sc::reservation_state_holds(reservation.state)) continue;
    for (const RackUnitInterval& interval : reservation.scope.units.intervals()) {
      spans.push_back(interval);
    }
  }
  for (std::size_t i = 0; i < spans.size(); ++i) {
    SC_CHECK(spans[i].is_valid());
    for (std::size_t j = i + 1; j < spans.size(); ++j) {
      SC_CHECK(!sc::intervals_overlap(spans[i], spans[j]));
    }
  }

  // available <= usable at every plane and every rack envelope.
  for (const SpaceNode& node : snapshot.nodes()) {
    if (node.own_planar.is_declared()) {
      const sc::AreaLedger area = snapshot.capacity_of(node.id, sc::FitContext{}).area;
      SC_CHECK(area.available <= area.usable);
      SC_CHECK(area.usable <= area.declared);
    }
    if (node.own_rack.is_declared() && sc::kind_may_declare_rack_envelope(node.kind)) {
      const sc::UnitLedger units = snapshot.capacity_of(node.id, sc::FitContext{}).units;
      SC_CHECK(units.available <= units.usable);
      SC_CHECK(units.usable <= units.declared);
    }
  }
}

struct StepTally final {
  std::int64_t applied = 0;
  std::int64_t refused = 0;
  std::int64_t illegal_attempts = 0;
  std::int64_t forced_refusals = 0;
};

void test_move_sequences(int cases) {
  SC_CASE("move_sequences");
  StepTally tally;
  const SpaceNodeId site = node_id("mv-site", 0, 0);

  for (int sequence = 0; sequence < cases; ++sequence) {
    sc_test::Rng rng(0x4D4F5645ull + static_cast<std::uint64_t>(sequence) * 1000003ull + 7u);
    const std::string store = identity_text("mv", sequence, 0);
    std::optional<sc::SpaceCapacityRegistry> registry = open_registry(store);
    Planner planner(*registry, store);

    std::vector<MoveNode> nodes;
    Grid grid(6, 6);

    const SpaceNodeId hall_a = node_id("mv-hall", sequence, 0);
    const SpaceNodeId hall_b = node_id("mv-hall", sequence, 1);
    const SpaceNodeId rack_a = node_id("mv-rack", sequence, 0);
    SC_CHECK(planner.create(
        make_node(site, SpaceNodeKind::site, SpatialClass::outdoor, SpaceNodeId{}, 0)));
    SC_CHECK(planner.create(make_node(hall_a, SpaceNodeKind::hall, SpatialClass::floor, site, 1)));
    SC_CHECK(planner.set_planar(hall_a, 6000, 6000));
    SC_CHECK(planner.create(make_node(hall_b, SpaceNodeKind::hall, SpatialClass::floor, site, 1)));
    SC_CHECK(planner.set_planar(hall_b, 6000, 6000));
    SC_CHECK(planner.create(make_node(rack_a, SpaceNodeKind::rack, SpatialClass::rack, hall_a, 2)));
    SC_CHECK(planner.set_rack_envelope(rack_a, kRackHeight));
    grid.take(Cell{0, 0});
    SC_CHECK(planner.set_placement(rack_a, cell_rect(Cell{0, 0})));
    SC_CHECK(planner.create_reservation(claim_id("mv-res", sequence, 0), rack_a,
                                        RackUnitInterval::of_count(1, 6)));

    MoveNode hall_a_entry;
    hall_a_entry.id = hall_a;
    hall_a_entry.kind = SpaceNodeKind::hall;
    hall_a_entry.parent = site;
    hall_a_entry.has_plane = true;
    MoveNode hall_b_entry;
    hall_b_entry.id = hall_b;
    hall_b_entry.kind = SpaceNodeKind::hall;
    hall_b_entry.parent = site;
    hall_b_entry.has_plane = true;
    MoveNode rack_a_entry;
    rack_a_entry.id = rack_a;
    rack_a_entry.kind = SpaceNodeKind::rack;
    rack_a_entry.parent = hall_a;
    rack_a_entry.has_cell = true;
    rack_a_entry.cell = Cell{0, 0};
    rack_a_entry.has_rack_envelope = true;
    nodes.push_back(hall_a_entry);
    nodes.push_back(hall_b_entry);
    nodes.push_back(rack_a_entry);

    const int steps = static_cast<int>(rng.between(3, 12));
    for (int step = 0; step < steps; ++step) {
      const std::uint64_t kind = rng.bounded(7);
      const std::size_t target = rng.bounded(static_cast<std::uint64_t>(nodes.size()));
      const std::size_t parent_index = rng.bounded(static_cast<std::uint64_t>(nodes.size()));
      const sc::SnapshotPtr before = registry->snapshot();
      const sc::RegistryRevision before_revision = before->revision();
      // One step may issue several requests - creating a node, then declaring
      // its envelope, then placing it - so the expected revision advance is the
      // number of requests that were accepted, counted here as they are made.
      std::uint64_t expected_advance = 0;
      const auto accept = [&expected_advance](bool ok) {
        if (ok) ++expected_advance;
        return ok;
      };

      if (kind == 0) {
        // Create a node. A rack is placed and gets a rack envelope so that it
        // consumes space; a hall declares a plane of its own.
        const bool want_plane = rng.bounded(2) == 0;
        const SpaceNodeId id = node_id("mv-new", sequence, static_cast<int>(nodes.size()));
        const SpaceNodeId parent = nodes[parent_index].id;
        const std::optional<Cell> cell = grid.take_random_free(rng);
        const SpaceNode created =
            make_node(id, want_plane ? SpaceNodeKind::hall : SpaceNodeKind::rack,
                      want_plane ? SpatialClass::floor : SpatialClass::rack, parent, 2);
        bool ok = accept(planner.create(created));
        if (ok && want_plane) ok = accept(planner.set_planar(id, 2000, 2000));
        if (ok && !want_plane) ok = accept(planner.set_rack_envelope(id, kRackHeight));
        if (ok && !want_plane && cell.has_value()) {
          ok = accept(planner.set_placement(id, cell_rect(*cell)));
        }
        if (ok) {
          MoveNode entry;
          entry.id = id;
          entry.kind = created.kind;
          entry.parent = parent;
          entry.has_plane = want_plane;
          entry.has_rack_envelope = !want_plane;
          if (!want_plane && cell.has_value()) {
            entry.has_cell = true;
            entry.cell = *cell;
          }
          nodes.push_back(entry);
        }
      } else if (kind == 1) {
        // A legal reparent: the new parent may contain the kind being moved.
        if (sc::kind_may_contain(nodes[parent_index].kind, nodes[target].kind)) {
          // A zero generation is a fence that can never match, so the library
          // must refuse it and leave the model alone.
          sc::ReparentNodeRequest fenced;
          fenced.node = nodes[target].id;
          fenced.new_parent = nodes[parent_index].id;
          fenced.precondition = sc::Precondition::at_generation(sc::EntityGeneration{0});
          const sc::Result<sc::MutationOutcome> hint = registry->apply(fenced);
          SC_CHECK(!hint.ok());
          if (!hint) ++tally.forced_refusals;

          sc::ReparentNodeRequest request;
          request.node = nodes[target].id;
          request.new_parent = nodes[parent_index].id;
          const sc::Result<sc::MutationOutcome> outcome = registry->apply(request);
          if (accept(outcome.ok())) nodes[target].parent = nodes[parent_index].id;
        }
      } else if (kind == 2) {
        // An illegal reparent: the node itself, or one of its descendants.
        std::vector<std::size_t> descendants;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
          if (i == target) continue;
          if (is_ancestor_in(nodes, nodes[target].id, nodes[i].id)) descendants.push_back(i);
        }
        std::size_t illegal_parent = target;
        if (!descendants.empty() && rng.bounded(2) == 0) {
          illegal_parent =
              descendants[rng.bounded(static_cast<std::uint64_t>(descendants.size()))];
        }
        sc::ReparentNodeRequest request;
        request.node = nodes[target].id;
        request.new_parent = nodes[illegal_parent].id;
        const sc::Result<sc::MutationOutcome> outcome = registry->apply(request);
        SC_CHECK(!outcome.ok());
        ++tally.illegal_attempts;
      } else if (kind == 3) {
        // Set an envelope: a plane on a node that may declare one, a rack
        // envelope on a rack.
        if (nodes[target].kind == SpaceNodeKind::rack) {
          accept(planner.set_rack_envelope(
              nodes[target].id, static_cast<std::int32_t>(rng.between(1, kRackHeight))));
        } else {
          const std::int64_t side = static_cast<std::int64_t>(rng.between(1, 4)) * 2000;
          accept(planner.set_planar(nodes[target].id, side, side));
        }
      } else if (kind == 4) {
        // Set or clear a placement on a cell no consuming record holds.
        if (rng.bounded(4) == 0) {
          const bool cleared = accept(planner.clear_placement(nodes[target].id));
          if (cleared) nodes[target].has_cell = false;
        } else {
          const std::optional<Cell> cell = grid.take_random_free(rng);
          if (cell.has_value()) {
            if (accept(planner.set_placement(nodes[target].id, cell_rect(*cell)))) {
              nodes[target].cell = *cell;
              nodes[target].has_cell = true;
            }
          }
        }
      } else if (kind == 5) {
        // Retirement. A node that is already retired, or a node whose lifecycle
        // does not allow the move, is refused; either way the model is left
        // consistent, which the checks after the step confirm.
        sc::RetireNodeRequest request;
        request.node = nodes[target].id;
        accept(registry->apply(request).ok());
      } else {
        // Replacement. The successor must exist and must not be a node the
        // replaced node contains; a cycle is refused by the library, which is
        // what this exercises.
        std::size_t successor = rng.bounded(static_cast<std::uint64_t>(nodes.size()));
        if (successor == target) successor = (target + 1) % nodes.size();
        sc::RetireNodeRequest request;
        request.node = nodes[target].id;
        request.successor = nodes[successor].id;
        const sc::Result<sc::MutationOutcome> outcome = registry->apply(request);
        accept(outcome.ok());
        if (!outcome && is_ancestor_in(nodes, nodes[target].id, nodes[successor].id)) {
          // The successor is inside the subtree being replaced: the library
          // must refuse it.
          SC_CHECK(!outcome.ok());
        }
      }

      const sc::SnapshotPtr after = registry->snapshot();
      SC_CHECK(after != nullptr);
      const std::uint64_t advance = after->revision().value() - before_revision.value();
      if (advance != 0) {
        // The step was accepted: every accepted request advanced the revision by
        // exactly one and the state really changed.
        if (advance != expected_advance) {
          std::fprintf(stderr, "TRACE sequence=%d step=%d kind=%llu advance=%llu expected=%llu\n",
                       sequence, step, static_cast<unsigned long long>(kind),
                       static_cast<unsigned long long>(advance),
                       static_cast<unsigned long long>(expected_advance));
        }
        SC_CHECK_EQ(advance, expected_advance);
        SC_CHECK_EQ(after->revision().value(), before_revision.value() + expected_advance);
        SC_CHECK(after->digest() != before->digest());
        ++tally.applied;
      } else {
        // The operation was refused: nothing was published, so the digest is
        // byte-identical to the one before the attempt.
        ++tally.refused;
        SC_CHECK(before->digest() == after->digest());
        SC_CHECK(before->canonical_bytes() == after->canonical_bytes());
        SC_CHECK_EQ(after->revision().value(), before_revision.value());
      }
      SC_CHECK(registry->audit().ok());
      check_tree_invariants(*after, site);
    }
  }
  std::fprintf(stdout,
               "move_sequences: %lld applied, %lld refused, %lld illegal attempts, "
               "%lld stale-generation fences refused\n",
               static_cast<long long>(tally.applied), static_cast<long long>(tally.refused),
               static_cast<long long>(tally.illegal_attempts),
               static_cast<long long>(tally.forced_refusals));
}

// ---------------------------------------------------------------------------
// 4. Ledger reconstruction over freshly built models.
// ---------------------------------------------------------------------------

void test_ledger_reconstruction(int cases) {
  SC_CASE("ledger_reconstruction");
  int accepted = 0;
  int refused = 0;
  for (int i = 0; i < cases; ++i) {
    const ModelPlan plan = make_model_plan(2000 + i);
    std::optional<sc::SpaceCapacityRegistry> registry = open_registry(plan.store);
    Planner planner(*registry, plan.name);
    if (!build_model(plan, planner)) {
      ++refused;
      continue;
    }
    ++accepted;
    const sc::SnapshotPtr snapshot = registry->snapshot();
    SC_CHECK(snapshot->audit().ok());
    std::int64_t declared_total = 0;
    std::int64_t available_total = 0;
    for (const SpaceNode& node : snapshot->nodes()) {
      if (!node.own_planar.is_declared()) continue;
      check_plane_ledger(*snapshot, node.id, false);
      const sc::NodeCapacity capacity = snapshot->capacity_of(node.id, sc::FitContext{});
      declared_total += capacity.area.declared.value();
      available_total += capacity.area.available.value();
    }
    const sc::CapacityRollup rollup = sc::model_rollup(*snapshot, sc::FitContext{});
    SC_CHECK_EQ(declared_total, rollup.area.declared.value());
    SC_CHECK_EQ(available_total, rollup.area.available.value());
  }
  std::fprintf(stdout, "ledger_reconstruction: %d models accepted, %d refused\n", accepted,
               refused);
}


// ---------------------------------------------------------------------------
// 5. Interval algebra.
// ---------------------------------------------------------------------------

void test_interval_algebra(int cases) {
  SC_CASE("interval_algebra");
  sc_test::Rng rng(0x1A7E4A11ull);
  int zero_free_cases = 0;
  for (int i = 0; i < cases; ++i) {
    const std::int32_t envelope = static_cast<std::int32_t>(rng.between(1, 64));
    const std::size_t count = static_cast<std::size_t>(rng.between(1, 8));
    std::vector<RackUnitInterval> requested;
    for (std::size_t k = 0; k < count; ++k) {
      const std::int32_t first = static_cast<std::int32_t>(rng.between(1, envelope));
      const std::int32_t last = static_cast<std::int32_t>(rng.between(first + 1, envelope + 1));
      requested.push_back(RackUnitInterval::make(first, last));
    }
    const sc::Result<sc::IntervalSet> built = sc::IntervalSet::build(requested);
    SC_CHECK(built.ok());
    if (!built) continue;
    const sc::IntervalSet occupied = built.value();

    // The occupied set is normalized: sorted, pairwise disjoint, in bounds.
    for (std::size_t k = 0; k < occupied.intervals().size(); ++k) {
      const RackUnitInterval& interval = occupied.intervals()[k];
      SC_CHECK(interval.is_valid());
      SC_CHECK(interval.first >= 1);
      SC_CHECK(interval.last <= envelope + 1);
      if (k > 0) SC_CHECK(occupied.intervals()[k - 1].last < interval.first);
    }

    const sc::Result<sc::IntervalSet> free_once = sc::IntervalSet::complement(occupied, envelope);
    SC_CHECK(free_once.ok());
    if (!free_once) continue;
    const sc::Result<sc::IntervalSet> free_twice =
        sc::IntervalSet::complement(free_once.value(), envelope);
    SC_CHECK(free_twice.ok());
    if (!free_twice) continue;

    // (a) the complement of the complement, unioned with the original, covers
    //     exactly [1, envelope + 1) with no overlap. The two sets may touch, so
    //     "ordered" means strictly increasing starts across the merged scan.
    // The bounds array is larger than the envelope on purpose: an interval that
    // legally reaches envelope + 1 must not write past the end.
    const std::size_t units = static_cast<std::size_t>(envelope) + 1;
    std::vector<int> covered(units + 1, 0);
    const auto scatter = [&](const std::vector<RackUnitInterval>& intervals) {
      for (const RackUnitInterval& interval : intervals) {
        SC_CHECK(interval.first >= 1);
        SC_CHECK(interval.last <= envelope + 1);
        SC_CHECK(interval.first < interval.last);
        for (std::int32_t unit = interval.first; unit < interval.last; ++unit) {
          covered[static_cast<std::size_t>(unit)] += 1;
        }
      }
    };
    // The complement of the complement is the set itself: complementing twice
    // recovers exactly the normalized input, so the operation is an involution
    // on normalized sets.
    SC_CHECK(free_twice.value() == occupied);
    // The set and its FIRST complement tile [1, envelope + 1) exactly once:
    // every unit is covered by precisely one of the two.
    scatter(free_once.value().intervals());
    scatter(occupied.intervals());
    bool tiles_exactly = true;
    for (std::int32_t unit = 1; unit <= envelope; ++unit) {
      if (covered[static_cast<std::size_t>(unit)] != 1) tiles_exactly = false;
    }
    SC_CHECK(tiles_exactly);
    // Both sets are normalized and in bounds.
    std::vector<RackUnitInterval> merged = free_once.value().intervals();
    merged.insert(merged.end(), occupied.intervals().begin(), occupied.intervals().end());
    std::sort(merged.begin(), merged.end());
    // Touching intervals are legal: occupied [1,3) and free [3,5) tile the
    // envelope without overlapping. Only a strict overlap is a defect.
    bool no_overlap = true;
    std::int32_t previous_last = 0;
    for (const RackUnitInterval& interval : merged) {
      if (interval.first < previous_last) no_overlap = false;
      previous_last = interval.last;
    }
    SC_CHECK(no_overlap);

    // (b) free_run_stats(occupied, envelope).total_free is the sum of the
    //     complement's interval counts.
    const sc::Result<sc::IntervalSet::FreeRunStats> stats =
        sc::IntervalSet::free_run_stats(occupied, envelope);
    SC_CHECK(stats.ok());
    if (!stats) continue;
    std::int32_t free_sum = 0;
    std::int32_t largest = 0;
    for (const RackUnitInterval& run : free_once.value().intervals()) {
      free_sum += run.count();
      largest = std::max(largest, run.count());
    }
    SC_CHECK_EQ(stats.value().total_free, free_sum);
    SC_CHECK_EQ(stats.value().largest, largest);
    SC_CHECK_EQ(stats.value().run_count,
                static_cast<std::uint32_t>(free_once.value().intervals().size()));
    SC_CHECK_EQ(free_sum, envelope - occupied.total().value.value());
    if (free_sum == 0) ++zero_free_cases;

    // (c) first_fit returns a run inside the complement, aligned, and clear of
    //     everything occupied - or a zero interval meaning "no such run".
    for (std::uint64_t trial = 0; trial < 3; ++trial) {
      const std::int32_t needed = static_cast<std::int32_t>(rng.between(1, envelope));
      const std::int32_t alignment = static_cast<std::int32_t>(rng.between(1, 8));
      const sc::Result<RackUnitInterval> fit =
          sc::IntervalSet::first_fit(occupied, envelope, needed, alignment);
      SC_CHECK(fit.ok());
      if (!fit) continue;
      if (fit.value().is_valid()) {
        SC_CHECK(free_once.value().covers(fit.value()));
        SC_CHECK_EQ(fit.value().count(), needed);
        SC_CHECK_EQ((fit.value().first - 1) % alignment, 0);
        SC_CHECK_EQ(fit.value().first, 1 + ((fit.value().first - 1) / alignment) * alignment);
        SC_CHECK(!occupied.intersects(fit.value()));
      } else {
        // A zero interval is a real "no": the reference search agrees.
        bool reference_found = false;
        for (const RackUnitInterval& run : free_once.value().intervals()) {
          if (run.count() < needed) continue;
          const std::int32_t offset = (run.first - 1) % alignment;
          const std::int32_t shift = offset == 0 ? 0 : alignment - offset;
          const std::int32_t start = run.first + shift;
          if (start + needed <= run.last) reference_found = true;
        }
        SC_CHECK(!reference_found);
      }
    }

    // (d) fragmentation_ppm is the definition, with integer division, and zero
    //     when nothing is free.
    const sc::Result<std::uint32_t> ppm = sc::IntervalSet::fragmentation_ppm(occupied, envelope);
    SC_CHECK(ppm.ok());
    if (!ppm) continue;
    if (free_sum == 0) {
      SC_CHECK_EQ(ppm.value(), 0u);
    } else {
      const std::int64_t expected = (static_cast<std::int64_t>(free_sum - largest) * 1000000) /
                                    static_cast<std::int64_t>(free_sum);
      SC_CHECK_EQ(static_cast<std::int64_t>(ppm.value()), expected);
    }
    const sc::Checked<sc::RackUnits> occupied_total = occupied.total();
    SC_CHECK(occupied_total.ok());
    SC_CHECK(occupied_total.value.value() + free_sum == envelope);
  }
  std::fprintf(stdout, "interval_algebra: %d interval sets checked (%d with no free unit)\n",
               cases, zero_free_cases);
}

// ---------------------------------------------------------------------------
// 6. Determinism.
// ---------------------------------------------------------------------------

void test_determinism(int cases) {
  SC_CASE("determinism");
  int identical = 0;
  int refused = 0;
  for (int i = 0; i < cases; ++i) {
    const ModelPlan plan = make_model_plan(3000 + i);
    std::optional<sc::SpaceCapacityRegistry> first = open_registry(plan.store);
    Planner first_planner(*first, plan.name);
    std::optional<sc::SpaceCapacityRegistry> second = open_registry(plan.store);
    Planner second_planner(*second, plan.name);
    const bool built_first = build_model(plan, first_planner);
    const bool built_second = build_model(plan, second_planner);
    SC_CHECK_EQ(built_first, built_second);
    if (!built_first || !built_second) {
      ++refused;
      continue;
    }
    ++identical;
    const sc::SnapshotPtr left = first->snapshot();
    const sc::SnapshotPtr right = second->snapshot();
    SC_CHECK_EQ(left->revision().value(), right->revision().value());
    SC_CHECK_EQ(left->node_count(), right->node_count());
    SC_CHECK(left->digest() == right->digest());
    SC_CHECK(left->canonical_text() == right->canonical_text());
    SC_CHECK(left->canonical_bytes() == right->canonical_bytes());
  }
  std::fprintf(stdout, "determinism: %d models built twice (%d refused)\n", identical, refused);
}

}  // namespace

int main() {
  std::fprintf(stdout, "space-capacity property test\n");

  // 1. Union area against a brute-force integer grid.
  test_union_area(300);

  // 2. Containment accounting and the rollup.
  const ModelTally tally = test_containment_accounting(200);
  SC_CHECK(tally.accepted > tally.refused * 4);
  SC_CHECK(tally.exact_identity > 0);
  test_whole_node_scope();

  // 3. Move, reparent and replacement sequences.
  test_move_sequences(150);

  // 4. Ledger reconstruction.
  test_ledger_reconstruction(80);

  // 5. Interval algebra.
  test_interval_algebra(400);

  // 6. Determinism.
  test_determinism(40);

  return ::sc_test::summary("property_test");
}
