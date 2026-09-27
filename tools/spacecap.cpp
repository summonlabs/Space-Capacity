// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - the inspection and administration command line tool.
//
// The tool is a thin shell over the library. It adds no validation of its own,
// so anything the CLI refuses the library refuses for the same reason, and
// anything the CLI accepts has passed exactly the library's checks. Every
// command prints deterministic text and returns a stable exit status.
//
// Exit statuses
//   0  the command succeeded
//   1  the command was refused, with a stable error code on stderr
//   2  the command line itself was malformed
//   9  reserved for the process-death injection harnesses

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/space_capacity/space_capacity.hpp"

namespace {

using namespace dccp::space_capacity;

constexpr int kExitOk = 0;
constexpr int kExitRefused = 1;
constexpr int kExitUsage = 2;

void usage() {
  std::fputs(
      "spacecap - physical-space capacity for the Data Center Control Plane\n"
      "\n"
      "usage: spacecap [global options] <command> [command options]\n"
      "\n"
      "global options\n"
      "  --state PATH        state file (default space-capacity.spcstate)\n"
      "  --store ID          store identity; derived from the file name when omitted\n"
      "  --actor NAME        operator identity recorded in the lock file\n"
      "  --source NAME       source identity recorded in the lock file\n"
      "  --read-only         open without write authority\n"
      "\n"
      "commands\n"
      "  version                                   print the version banner\n"
      "  status                                    print the store status\n"
      "  verify                                    verify the durable state and audit it\n"
      "  show                                      print the canonical text of the state\n"
      "  summary                                   print the whole-model rollup\n"
      "  report    --node ID                       print the capacity report of one node\n"
      "  fit-units --rack ID --units N             assess a rack unit fit\n"
      "              [--alignment N] [--candidates N] [--count-pending]\n"
      "  fit-rect  --node ID --width MM --depth MM assess a planar rectangle fit\n"
      "              [--alignment MM] [--candidates N] [--count-pending]\n"
      "  diff      --from REV --to REV             print the diff between two revisions\n"
      "  node-add  --id ID --kind KIND --class CLASS\n"
      "              [--parent ID] [--label TEXT] [--area MM2] [--width MM --depth MM]\n"
      "              [--rack-units N] [--x MM --y MM]\n"
      "  node-lifecycle --id ID --lifecycle STATE [--successor ID]\n"
      "  node-reparent  --id ID --parent ID\n"
      "  claim-add --id ID --node ID --state STATE\n"
      "              [--units FIRST-LAST] [--rect X,Y,W,H] [--occupant KIND] [--allow-pending]\n"
      "  claim-state --id ID --state STATE\n"
      "  exclusion-add --id ID --node ID --reason REASON [--rect X,Y,W,H] [--units FIRST-LAST]\n"
      "                  [--state STATE]\n"
      "  lease                                     take the writer lease and hold it until\n"
      "                                            standard input reaches end of file\n"
      "\n"
      "A query never grants placement, and this tool never decides one.\n",
      stderr);
}

struct Options final {
  std::string state = std::string(kDefaultStateFile);
  std::string store;
  std::string actor = "cli";
  std::string source = "spacecap";
  bool read_only = false;
  std::string command;
  std::vector<std::pair<std::string, std::string>> arguments;
};

bool parse_options(int argc, char** argv, Options& options) {
  int index = 1;
  while (index < argc) {
    const std::string_view token(argv[index]);
    if (token == "--state" || token == "--store" || token == "--actor" || token == "--source") {
      if (index + 1 >= argc) return false;
      const std::string value(argv[index + 1]);
      if (token == "--state") options.state = value;
      if (token == "--store") options.store = value;
      if (token == "--actor") options.actor = value;
      if (token == "--source") options.source = value;
      index += 2;
      continue;
    }
    if (token == "--read-only") {
      options.read_only = true;
      ++index;
      continue;
    }
    break;
  }
  if (index >= argc) return false;
  options.command = argv[index];
  ++index;
  while (index < argc) {
    const std::string_view token(argv[index]);
    if (token.rfind("--", 0) != 0) return false;
    if (index + 1 >= argc) return false;
    options.arguments.emplace_back(std::string(token.substr(2)), std::string(argv[index + 1]));
    index += 2;
  }
  return true;
}

const std::string* find(const Options& options, std::string_view name) {
  for (const auto& entry : options.arguments) {
    if (entry.first == name) return &entry.second;
  }
  return nullptr;
}

std::string require(const Options& options, std::string_view name, bool& ok) {
  const std::string* value = find(options, name);
  if (value == nullptr) {
    std::fprintf(stderr, "spacecap: --%s is required\n", std::string(name).c_str());
    ok = false;
    return {};
  }
  return *value;
}

bool parse_u64_arg(const Options& options, std::string_view name, std::uint64_t& out) {
  const std::string* value = find(options, name);
  if (value == nullptr) return false;
  Result<std::uint64_t> parsed = parse_u64(*value);
  if (!parsed) return false;
  out = parsed.value();
  return true;
}

bool parse_i64_arg(const Options& options, std::string_view name, std::int64_t& out) {
  const std::string* value = find(options, name);
  if (value == nullptr) return false;
  Result<std::int64_t> parsed = parse_i64(*value);
  if (!parsed) return false;
  out = parsed.value();
  return true;
}

// Parses "X,Y,W,H" in whole millimetres.
bool parse_rect(std::string_view text, PlanarRect& out) {
  std::int64_t values[4] = {0, 0, 0, 0};
  std::size_t field = 0;
  std::size_t begin = 0;
  while (field < 4) {
    const std::size_t comma = text.find(',', begin);
    const std::string_view piece =
        comma == std::string_view::npos ? text.substr(begin) : text.substr(begin, comma - begin);
    Result<std::int64_t> parsed = parse_i64(piece);
    if (!parsed) return false;
    values[field] = parsed.value();
    ++field;
    if (comma == std::string_view::npos) break;
    begin = comma + 1;
  }
  if (field != 4) return false;
  out = PlanarRect::make(values[0], values[1], values[2], values[3]);
  return true;
}

// Parses "FIRST-LAST" as a half-open rack unit interval.
bool parse_span(std::string_view text, RackUnitInterval& out) {
  const std::size_t dash = text.find('-');
  if (dash == std::string_view::npos) return false;
  Result<std::int64_t> first = parse_i64(text.substr(0, dash));
  Result<std::int64_t> last = parse_i64(text.substr(dash + 1));
  if (!first || !last) return false;
  if (first.value() < 1 || last.value() > Limits::kMaxRackUnits + 1) return false;
  out = RackUnitInterval::make(static_cast<std::int32_t>(first.value()),
                               static_cast<std::int32_t>(last.value()));
  return out.is_valid();
}

int fail(const Error& error) {
  std::fprintf(stderr, "refused: %s\n", error.to_string().c_str());
  return kExitRefused;
}

template <typename T>
int fail(const Result<T>& result) {
  return fail(result.error());
}

int fail(const Status& status) {
  std::fprintf(stderr, "refused: %s\n", status.error().to_string().c_str());
  return kExitRefused;
}

void print_explanations(const ExplanationSet& explanations) {
  for (const Explanation& explanation : explanations.items()) {
    std::printf("  explanation %s\n", explanation.to_string().c_str());
  }
}

void print_rollup(const CapacityRollup& rollup) {
  std::printf("revision %s\n", rollup.revision.to_string().c_str());
  std::printf("nodes %llu planes %llu racks %llu\n",
              static_cast<unsigned long long>(rollup.node_count),
              static_cast<unsigned long long>(rollup.plane_owner_count),
              static_cast<unsigned long long>(rollup.rack_count));
  std::printf("records claims %llu reservations %llu exclusions %llu clearances %llu zones %llu\n",
              static_cast<unsigned long long>(rollup.claim_count),
              static_cast<unsigned long long>(rollup.reservation_count),
              static_cast<unsigned long long>(rollup.exclusion_count),
              static_cast<unsigned long long>(rollup.clearance_count),
              static_cast<unsigned long long>(rollup.expansion_zone_count));
  std::printf("area state %s\n", std::string(measure_state_name(rollup.area.state)).c_str());
  std::printf("area declared %s\n", to_text(rollup.area.declared).c_str());
  std::printf("area excluded %s\n", to_text(rollup.area.excluded).c_str());
  std::printf("area usable %s\n", to_text(rollup.area.usable).c_str());
  std::printf("area structural %s\n", to_text(rollup.area.structural).c_str());
  std::printf("area claimed %s\n", to_text(rollup.area.claimed).c_str());
  std::printf("area held %s\n", to_text(rollup.area.held).c_str());
  std::printf("area earmarked %s\n", to_text(rollup.area.earmarked).c_str());
  std::printf("area pending %s\n", to_text(rollup.area.pending).c_str());
  std::printf("area planned %s\n", to_text(rollup.area.planned).c_str());
  std::printf("area available %s\n", to_text(rollup.area.available).c_str());
  std::printf("area over-committed %s\n", rollup.area.over_committed ? "true" : "false");
  std::printf("units state %s\n", std::string(measure_state_name(rollup.units.state)).c_str());
  std::printf("units declared %s\n", to_text(rollup.units.declared).c_str());
  std::printf("units excluded %s\n", to_text(rollup.units.excluded).c_str());
  std::printf("units usable %s\n", to_text(rollup.units.usable).c_str());
  std::printf("units occupied %s\n", to_text(rollup.units.occupied).c_str());
  std::printf("units pending %s\n", to_text(rollup.units.pending).c_str());
  std::printf("units available %s\n", to_text(rollup.units.available).c_str());
  std::printf("units free-runs %u\n", rollup.units.free_runs);
  std::printf("units largest-free-run %s\n", to_text(rollup.units.largest_free_run).c_str());
  std::printf("units fragmentation-ppm %u\n", rollup.units.fragmentation_ppm);
  std::printf("flags undeclared-envelopes %s inactive-nodes %s over-committed %s\n",
              rollup.contains_undeclared_envelopes ? "true" : "false",
              rollup.contains_inactive_nodes ? "true" : "false",
              rollup.over_committed ? "true" : "false");
}

bool parse_context(const Options& options, FitContext& context) {
  const std::string* occupant = find(options, "occupant");
  if (occupant != nullptr) {
    OccupantKind kind = OccupantKind::unknown;
    if (!parse_occupant_kind(*occupant, kind)) return false;
    context.occupant = kind;
  }
  if (find(options, "count-pending") != nullptr || find(options, "at") != nullptr) {
    context.count_pending = true;
  }
  const std::string* at = find(options, "now");
  if (at != nullptr) {
    Timestamp stamp;
    if (!parse_timestamp(*at, stamp)) return false;
    context.now = stamp;
  }
  return true;
}

int make_store_options(const Options& options, StoreOptions& store_options) {
  store_options.path = options.state;
  store_options.actor = options.actor;
  store_options.source = options.source;
  store_options.mode = options.read_only ? OpenMode::read_only : OpenMode::read_write;
  if (!options.store.empty()) {
    Result<StoreId> identity = StoreId::parse(options.store);
    if (!identity) return fail(identity);
    store_options.store_identity = identity.value();
  }
  return kExitOk;
}

// Builds one node from the command line. Every field is validated by the
// library on apply, so nothing here duplicates a rule.
Result<SpaceNode> node_from(const Options& options, const SnapshotPtr& snapshot) {
  bool ok = true;
  const std::string id = require(options, "id", ok);
  const std::string kind = require(options, "kind", ok);
  const std::string spatial = require(options, "class", ok);
  if (!ok) return Error::make(ErrorCode::invalid_argument, "missing required option");

  Result<SpaceNodeId> node_id = SpaceNodeId::parse(id);
  if (!node_id) return node_id.error();
  SpaceNodeKind node_kind = SpaceNodeKind::none;
  if (!parse_space_node_kind(kind, node_kind)) {
    return Error::make(ErrorCode::unknown_enum_token, "unknown node kind " + kind);
  }
  SpatialClass spatial_class = SpatialClass::unspecified;
  if (!parse_spatial_class(spatial, spatial_class)) {
    return Error::make(ErrorCode::unknown_enum_token, "unknown spatial class " + spatial);
  }

  SpaceNode node;
  node.id = node_id.value();
  node.generation = EntityGeneration{1};
  node.kind = node_kind;
  node.spatial_class = spatial_class;
  node.lifecycle = NodeLifecycle::available;

  const std::string* parent = find(options, "parent");
  if (parent != nullptr && !parent->empty()) {
    Result<SpaceNodeId> parsed = SpaceNodeId::parse(*parent);
    if (!parsed) return parsed.error();
    node.parent = parsed.value();
  }
  // The depth is the parent's depth plus one. The CLI resolves it from the
  // committed model rather than asking the operator to state it, because a
  // depth the operator typed could disagree with the tree.
  if (!node.parent.empty()) {
    const SpaceNode* parent_node = snapshot->find_node(node.parent);
    if (parent_node == nullptr) {
      return Error::with_subject(ErrorCode::not_found, "the named parent is not in the model",
                                 node.parent.str());
    }
    node.depth = parent_node->depth + 1;
  }
  const std::string* label = find(options, "label");
  if (label != nullptr) {
    Result<DisplayLabel> parsed = DisplayLabel::parse(*label);
    if (!parsed) return parsed.error();
    node.label = parsed.value();
  }

  std::uint64_t area = 0;
  if (parse_u64_arg(options, "area", area)) {
    node.own_planar.declared_area = SquareMillimeters{static_cast<std::int64_t>(area)};
  }
  std::int64_t width = 0;
  std::int64_t depth = 0;
  const bool has_width = parse_i64_arg(options, "width", width);
  const bool has_depth = parse_i64_arg(options, "depth", depth);
  if (has_width || has_depth) {
    if (!has_width || !has_depth) {
      return Error::make(ErrorCode::invalid_argument, "both --width and --depth are required");
    }
    Result<RectSet> rects = RectSet::build({PlanarRect::make(0, 0, width, depth)});
    if (!rects) return rects.error();
    const Checked<SquareMillimeters> measured = rects.value().total_area();
    if (!measured) {
      return Error::make(ErrorCode::arithmetic_overflow, "the declared area overflowed");
    }
    node.own_planar.rects = std::move(rects).value();
    if (node.own_planar.declared_area.is_zero()) {
      node.own_planar.declared_area = measured.value;
    }
  }
  std::uint64_t rack_units = 0;
  if (parse_u64_arg(options, "rack-units", rack_units)) {
    node.own_rack.height = RackUnits{static_cast<std::int32_t>(rack_units)};
  }
  // Where the node sits inside its parent. A node that declares a plane inside
  // another plane must state a placement whose area equals its declared area,
  // and the library enforces that on apply.
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t rect_width = 0;
  std::int64_t rect_depth = 0;
  const bool has_x = parse_i64_arg(options, "x", x);
  const bool has_y = parse_i64_arg(options, "y", y);
  const bool has_rect_width = parse_i64_arg(options, "placement-width", rect_width);
  const bool has_rect_depth = parse_i64_arg(options, "placement-depth", rect_depth);
  if (has_x || has_y || has_rect_width || has_rect_depth) {
    if (!has_x || !has_y || !has_rect_width || !has_rect_depth) {
      return Error::make(ErrorCode::invalid_argument,
                         "a placement needs --x, --y, --placement-width and --placement-depth");
    }
    node.placement.has_base_rect = true;
    node.placement.base_rect = PlanarRect::make(x, y, rect_width, rect_depth);
  }
  const std::string* span = find(options, "span");
  if (span != nullptr) {
    RackUnitInterval interval;
    if (!parse_span(*span, interval)) {
      return Error::make(ErrorCode::invalid_extent, "malformed --span, expected FIRST-LAST");
    }
    node.placement.has_u_span = true;
    node.placement.u_span = interval;
  }
  return node;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    usage();
    return kExitUsage;
  }

  if (options.command == "version") {
    std::printf("%s\n", std::string(version_banner()).c_str());
    std::printf("store format version %u, model revision %u\n", kStoreFormatVersion, kModelRevision);
    return kExitOk;
  }
  if (options.command == "help" || options.command == "--help") {
    usage();
    return kExitOk;
  }

  StoreOptions store_options;
  const int prepared = make_store_options(options, store_options);
  if (prepared != kExitOk) return prepared;

  // Read-only commands open read-only so that an inspection can never write.
  const bool mutating = options.command == "node-add" || options.command == "node-lifecycle" ||
                        options.command == "node-reparent" || options.command == "claim-add" ||
                        options.command == "claim-state" || options.command == "exclusion-add" ||
                        options.command == "lease";
  if (!mutating) store_options.mode = OpenMode::read_only;

  Result<SpaceCapacityRegistry> opened = SpaceCapacityRegistry::open(store_options);
  if (!opened) return fail(opened.error());
  SpaceCapacityRegistry registry = std::move(opened).value();

  if (options.command == "status") {
    Result<StoreStatus> status = registry.store_status();
    if (!status) return fail(status);
    std::printf("state %s\n", status.value().path.string().c_str());
    std::printf("identity %s\n", status.value().identity_path.string().c_str());
    std::printf("store %s\n", status.value().store.str().c_str());
    std::printf("incarnation %s\n", status.value().incarnation.to_string().c_str());
    std::printf("revision %s\n", status.value().revision.to_string().c_str());
    std::printf("attempt %s\n", status.value().attempt.to_string().c_str());
    std::printf("digest %s\n", status.value().digest.tagged_hex().c_str());    std::printf("state-bytes %llu previous-bytes %llu\n",
                static_cast<unsigned long long>(status.value().state_bytes),
                static_cast<unsigned long long>(status.value().previous_bytes));
    std::printf("state-present %s previous-present %s identity-present %s\n",
                status.value().state_exists ? "true" : "false",
                status.value().previous_exists ? "true" : "false",
                status.value().identity_exists ? "true" : "false");
    return kExitOk;
  }

  if (options.command == "verify") {
    const Status verified = registry.verify();
    if (!verified) return fail(verified);
    std::printf("verified revision %s digest %s\n", registry.revision().to_string().c_str(),
                registry.snapshot()->digest().tagged_hex().c_str());
    return kExitOk;
  }

  if (options.command == "show") {
    std::string text = registry.snapshot()->canonical_text();
    std::fwrite(text.data(), 1, text.size(), stdout);
    return kExitOk;
  }

  if (options.command == "summary") {
    FitContext context;
    if (!parse_context(options, context)) {
      std::fputs("spacecap: malformed fit context\n", stderr);
      return kExitUsage;
    }
    const std::string* node = find(options, "node");
    if (node == nullptr) {
      print_rollup(registry.model_rollup(context));
      return kExitOk;
    }
    Result<SpaceNodeId> id = SpaceNodeId::parse(*node);
    if (!id) return fail(id);
    Result<CapacityRollup> rollup = registry.rollup(id.value(), context);
    if (!rollup) return fail(rollup);
    print_rollup(rollup.value());
    return kExitOk;
  }

  if (options.command == "report") {
    bool ok = true;
    const std::string node = require(options, "node", ok);
    if (!ok) return kExitUsage;
    Result<SpaceNodeId> id = SpaceNodeId::parse(node);
    if (!id) return fail(id);
    FitContext context;
    if (!parse_context(options, context)) return kExitUsage;
    Result<CapacityReport> report = registry.report(id.value(), context);
    if (!report) return fail(report);
    std::printf("subject %s generation %s lifecycle %s\n", report.value().subject.str().c_str(),
                report.value().subject_generation.to_string().c_str(),
                std::string(node_lifecycle_name(report.value().lifecycle)).c_str());
    std::printf("attributed-plane %s owns-plane %s owns-rack-envelope %s\n",
                report.value().attributed_plane.empty() ? "-" : report.value().attributed_plane.str().c_str(),
                report.value().owns_plane ? "true" : "false",
                report.value().owns_rack_envelope ? "true" : "false");
    std::printf("area state %s declared %s excluded %s usable %s structural %s claimed %s\n",
                std::string(measure_state_name(report.value().area.state)).c_str(),
                to_text(report.value().area.declared).c_str(),
                to_text(report.value().area.excluded).c_str(),
                to_text(report.value().area.usable).c_str(),
                to_text(report.value().area.structural).c_str(),
                to_text(report.value().area.claimed).c_str());
    std::printf("area held %s earmarked %s pending %s planned %s available %s over-committed %s\n",
                to_text(report.value().area.held).c_str(),
                to_text(report.value().area.earmarked).c_str(),
                to_text(report.value().area.pending).c_str(),
                to_text(report.value().area.planned).c_str(),
                to_text(report.value().area.available).c_str(),
                report.value().area.over_committed ? "true" : "false");
    std::printf("units state %s declared %s excluded %s usable %s occupied %s available %s\n",
                std::string(measure_state_name(report.value().units.state)).c_str(),
                to_text(report.value().units.declared).c_str(),
                to_text(report.value().units.excluded).c_str(),
                to_text(report.value().units.usable).c_str(),
                to_text(report.value().units.occupied).c_str(),
                to_text(report.value().units.available).c_str());
    std::printf("units free-runs %u largest-free-run %s fragmentation-ppm %u\n",
                report.value().units.free_runs,
                to_text(report.value().units.largest_free_run).c_str(),
                report.value().units.fragmentation_ppm);
    for (const NodeCapacity& envelope : report.value().planes) {
      std::printf("envelope %s generation %s plane %s declared %s available %s units %s\n",
                  envelope.node.str().c_str(), envelope.generation.to_string().c_str(),
                  envelope.owns_plane ? "yes" : "no", to_text(envelope.area.declared).c_str(),
                  to_text(envelope.area.available).c_str(),
                  to_text(envelope.units.declared).c_str());
    }
    print_explanations(report.value().explanations);
    return kExitOk;
  }

  if (options.command == "fit-units") {
    bool ok = true;
    const std::string rack = require(options, "rack", ok);
    const std::string units = require(options, "units", ok);
    if (!ok) return kExitUsage;
    Result<SpaceNodeId> rack_id = SpaceNodeId::parse(rack);
    if (!rack_id) return fail(rack_id);
    Result<std::uint64_t> needed = parse_u64(units);
    if (!needed) return fail(needed);
    RackUnitFitRequest request;
    request.rack = rack_id.value();
    request.needed = RackUnits{static_cast<std::int32_t>(needed.value())};
    std::uint64_t alignment = 0;
    if (parse_u64_arg(options, "alignment", alignment)) {
      request.alignment = static_cast<std::int32_t>(alignment);
    }
    std::uint64_t candidates = 0;
    if (parse_u64_arg(options, "candidates", candidates)) {
      request.max_candidates = static_cast<std::uint32_t>(candidates);
    }
    if (!parse_context(options, request.context)) return kExitUsage;
    Result<FitAssessment> assessment = registry.assess(request);
    if (!assessment) return fail(assessment);
    std::printf("verdict %s\n",
                std::string(fit_verdict_name(assessment.value().verdict)).c_str());
    std::printf("revision %s attempt %s\n", assessment.value().revision.to_string().c_str(),
                assessment.value().attempt.to_string().c_str());
    std::printf("declared %s usable %s occupied %s available %s free-runs %u largest %s ppm %u\n",
                to_text(assessment.value().units.declared).c_str(),
                to_text(assessment.value().units.usable).c_str(),
                to_text(assessment.value().units.occupied).c_str(),
                to_text(assessment.value().units.available).c_str(),
                assessment.value().units.free_runs,
                to_text(assessment.value().units.largest_free_run).c_str(),
                assessment.value().units.fragmentation_ppm);
    for (const PlacementCandidate& candidate : assessment.value().candidates) {
      std::printf("candidate %s\n", to_text(candidate.units).c_str());
    }
    print_explanations(assessment.value().explanations);
    return kExitOk;
  }

  if (options.command == "fit-rect") {
    bool ok = true;
    const std::string node = require(options, "node", ok);
    const std::string width = require(options, "width", ok);
    const std::string depth = require(options, "depth", ok);
    if (!ok) return kExitUsage;
    Result<SpaceNodeId> node_id = SpaceNodeId::parse(node);
    if (!node_id) return fail(node_id);
    Result<std::int64_t> w = parse_i64(width);
    Result<std::int64_t> d = parse_i64(depth);
    if (!w) return fail(w);
    if (!d) return fail(d);
    PlanarRectFitRequest request;
    request.node = node_id.value();
    request.width = Millimeters{w.value()};
    request.depth = Millimeters{d.value()};
    std::int64_t alignment = 0;
    if (parse_i64_arg(options, "alignment", alignment)) {
      request.alignment = Millimeters{alignment};
    }
    std::uint64_t candidates = 0;
    if (parse_u64_arg(options, "candidates", candidates)) {
      request.max_candidates = static_cast<std::uint32_t>(candidates);
    }
    if (!parse_context(options, request.context)) return kExitUsage;
    Result<FitAssessment> assessment = registry.assess(request);
    if (!assessment) return fail(assessment);
    std::printf("verdict %s\n",
                std::string(fit_verdict_name(assessment.value().verdict)).c_str());
    std::printf("plane %s available %s\n", assessment.value().attributed_plane.str().c_str(),
                to_text(assessment.value().area.available).c_str());
    for (const PlacementCandidate& candidate : assessment.value().candidates) {
      std::printf("candidate %s\n", to_text(candidate.rect).c_str());
    }
    print_explanations(assessment.value().explanations);
    return kExitOk;
  }

  if (options.command == "diff") {
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    if (!parse_u64_arg(options, "from", from) || !parse_u64_arg(options, "to", to)) {
      std::fputs("spacecap: --from and --to are required\n", stderr);
      return kExitUsage;
    }
    Result<CapacityDiff> diff =
        registry.diff(RegistryRevision{from}, RegistryRevision{to});
    if (!diff) return fail(diff);
    const CapacityDiff& value = diff.value();
    std::printf("from %s to %s\n", value.from_revision.to_string().c_str(),
                value.to_revision.to_string().c_str());
    std::printf("changes %llu\n", static_cast<unsigned long long>(value.change_count()));
    for (const NodeChange& change : value.nodes) {
      std::printf("node %s %s", change.id.str().c_str(),
                  std::string(change_kind_name(change.kind)).c_str());
      for (const std::string& field : change.fields) std::printf(" %s", field.c_str());
      std::printf("\n");
    }
    for (const ClaimChange& change : value.claims) {
      std::printf("claim %s %s", change.id.str().c_str(),
                  std::string(change_kind_name(change.kind)).c_str());
      for (const std::string& field : change.fields) std::printf(" %s", field.c_str());
      std::printf("\n");
    }
    std::printf("area-available-delta %lld units-available-delta %lld\n",
                static_cast<long long>(value.area_delta.available),
                static_cast<long long>(value.unit_delta.available));
    print_explanations(value.explanations);
    return kExitOk;
  }

  if (options.command == "node-add") {
    Result<SpaceNode> node = node_from(options, registry.snapshot());
    if (!node) return fail(node);
    CreateNodeRequest request;
    request.node = node.value();
    const std::string* key = find(options, "request-id");
    if (key != nullptr) {
      Result<RequestId> parsed = RequestId::parse(*key);
      if (!parsed) return fail(parsed);
      request.request_id = parsed.value();
    }
    Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) return fail(outcome);
    std::printf("applied %s revision %s attempt %s digest %s\n",
                outcome.value().applied ? "true" : "false",
                outcome.value().revision.to_string().c_str(),
                outcome.value().attempt.to_string().c_str(),
                outcome.value().state_digest.tagged_hex().c_str());
    print_explanations(outcome.value().explanations);
    return kExitOk;
  }

  if (options.command == "node-lifecycle") {
    bool ok = true;
    const std::string node = require(options, "id", ok);
    const std::string lifecycle = require(options, "lifecycle", ok);
    if (!ok) return kExitUsage;
    Result<SpaceNodeId> id = SpaceNodeId::parse(node);
    if (!id) return fail(id);
    NodeLifecycle state = NodeLifecycle::planned;
    if (!parse_node_lifecycle(lifecycle, state)) {
      std::fprintf(stderr, "spacecap: unknown lifecycle %s\n", lifecycle.c_str());
      return kExitUsage;
    }
    SetNodeLifecycleRequest request;
    request.node = id.value();
    request.lifecycle = state;
    const std::string* successor = find(options, "successor");
    if (successor != nullptr) {
      Result<SpaceNodeId> parsed = SpaceNodeId::parse(*successor);
      if (!parsed) return fail(parsed);
      request.successor = parsed.value();
    }
    Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) return fail(outcome);
    std::printf("applied revision %s\n", outcome.value().revision.to_string().c_str());
    return kExitOk;
  }

  if (options.command == "node-reparent") {
    bool ok = true;
    const std::string node = require(options, "id", ok);
    const std::string parent = require(options, "parent", ok);
    if (!ok) return kExitUsage;
    Result<SpaceNodeId> id = SpaceNodeId::parse(node);
    Result<SpaceNodeId> parent_id = SpaceNodeId::parse(parent);
    if (!id) return fail(id);
    if (!parent_id) return fail(parent_id);
    ReparentNodeRequest request;
    request.node = id.value();
    request.new_parent = parent_id.value();
    Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) return fail(outcome);
    std::printf("applied revision %s\n", outcome.value().revision.to_string().c_str());
    return kExitOk;
  }

  if (options.command == "claim-add") {
    bool ok = true;
    const std::string id_text = require(options, "id", ok);
    const std::string node_text = require(options, "node", ok);
    const std::string state_text = require(options, "state", ok);
    if (!ok) return kExitUsage;
    Result<OccupancyClaimId> id = OccupancyClaimId::parse(id_text);
    Result<SpaceNodeId> node = SpaceNodeId::parse(node_text);
    if (!id) return fail(id);
    if (!node) return fail(node);
    ClaimState state = ClaimState::planned;
    if (!parse_claim_state(state_text, state)) {
      std::fprintf(stderr, "spacecap: unknown claim state %s\n", state_text.c_str());
      return kExitUsage;
    }
    CreateClaimRequest request;
    request.claim.id = id.value();
    request.claim.generation = EntityGeneration{1};
    request.claim.node = node.value();
    request.claim.state = state;
    const std::string* occupant = find(options, "occupant");
    if (occupant != nullptr) {
      OccupantKind kind = OccupantKind::unknown;
      if (!parse_occupant_kind(*occupant, kind)) {
        std::fprintf(stderr, "spacecap: unknown occupant %s\n", occupant->c_str());
        return kExitUsage;
      }
      request.claim.occupant = kind;
    }
    const std::string* span = find(options, "units");
    const std::string* rect = find(options, "rect");
    if (span != nullptr && rect != nullptr) {
      std::fputs("spacecap: --units and --rect are mutually exclusive\n", stderr);
      return kExitUsage;
    }
    if (span != nullptr) {
      RackUnitInterval interval;
      if (!parse_span(*span, interval)) {
        std::fputs("spacecap: malformed --units, expected FIRST-LAST\n", stderr);
        return kExitUsage;
      }
      Result<IntervalSet> units = IntervalSet::build({interval});
      if (!units) return fail(units);
      request.claim.scope.kind = FootprintScopeKind::rack_units;
      request.claim.scope.units = units.value();
    } else if (rect != nullptr) {
      PlanarRect parsed;
      if (!parse_rect(*rect, parsed)) {
        std::fputs("spacecap: malformed --rect, expected X,Y,W,H\n", stderr);
        return kExitUsage;
      }
      Result<RectSet> rects = RectSet::build({parsed});
      if (!rects) return fail(rects);
      request.claim.scope.kind = FootprintScopeKind::planar;
      request.claim.scope.rects = rects.value();
    }
    Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) return fail(outcome);
    std::printf("applied revision %s\n", outcome.value().revision.to_string().c_str());
    print_explanations(outcome.value().explanations);
    return kExitOk;
  }

  if (options.command == "claim-state") {
    bool ok = true;
    const std::string id_text = require(options, "id", ok);
    const std::string state_text = require(options, "state", ok);
    if (!ok) return kExitUsage;
    Result<OccupancyClaimId> id = OccupancyClaimId::parse(id_text);
    if (!id) return fail(id);
    ClaimState state = ClaimState::planned;
    if (!parse_claim_state(state_text, state)) {
      std::fprintf(stderr, "spacecap: unknown claim state %s\n", state_text.c_str());
      return kExitUsage;
    }
    TransitionClaimRequest request;
    request.claim = id.value();
    request.next = state;
    Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) return fail(outcome);
    std::printf("applied revision %s\n", outcome.value().revision.to_string().c_str());
    return kExitOk;
  }

  if (options.command == "exclusion-add") {
    bool ok = true;
    const std::string id_text = require(options, "id", ok);
    const std::string node_text = require(options, "node", ok);
    const std::string reason_text = require(options, "reason", ok);
    if (!ok) return kExitUsage;
    Result<ExclusionRegionId> id = ExclusionRegionId::parse(id_text);
    Result<SpaceNodeId> node = SpaceNodeId::parse(node_text);
    if (!id) return fail(id);
    if (!node) return fail(node);
    ExclusionReason reason = ExclusionReason::unspecified;
    if (!parse_exclusion_reason(reason_text, reason)) {
      std::fprintf(stderr, "spacecap: unknown exclusion reason %s\n", reason_text.c_str());
      return kExitUsage;
    }
    CreateExclusionRequest request;
    request.region.id = id.value();
    request.region.generation = EntityGeneration{1};
    request.region.node = node.value();
    request.region.reason = reason;
    request.region.state = ExclusionState::active;
    request.region.blocks = default_mask_for_reason(reason);
    const std::string* rect = find(options, "rect");
    if (rect != nullptr) {
      PlanarRect parsed;
      if (!parse_rect(*rect, parsed)) return kExitUsage;
      Result<RectSet> rects = RectSet::build({parsed});
      if (!rects) return fail(rects);
      request.region.scope.kind = FootprintScopeKind::planar;
      request.region.scope.rects = rects.value();
    }
    Result<MutationOutcome> outcome = registry.apply(request);
    if (!outcome) return fail(outcome);
    std::printf("applied revision %s\n", outcome.value().revision.to_string().c_str());
    return kExitOk;
  }

  if (options.command == "lease") {
    const Status acquired = registry.acquire_writer_lease();
    if (!acquired) return fail(acquired);
    std::printf("lease-held\n");
    std::fflush(stdout);
    // Hold the lease until standard input reaches end of file. The writer lock
    // is an operating-system lock, so killing this process releases it.
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
      if (std::string_view(buffer).rfind("release", 0) == 0) break;
    }
    const Status released = registry.release_writer_lease();
    if (!released) return fail(released);
    std::printf("lease-released\n");
    return kExitOk;
  }

  std::fprintf(stderr, "spacecap: unknown command %s\n", options.command.c_str());
  usage();
  return kExitUsage;
}
