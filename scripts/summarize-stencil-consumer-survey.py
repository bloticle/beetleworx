"""Summarize the CPU-only native S8 lifetime survey; never infer an FPS gain."""
import argparse
from collections import defaultdict
import json
from pathlib import Path

COMPARE = ("never", "less", "equal", "less_equal", "greater", "not_equal", "greater_equal", "always")
OPS = ("keep", "zero", "replace", "increment_clamp", "decrement_clamp", "invert", "increment_wrap", "decrement_wrap")
PRIMITIVES = {0: "none", 1: "point_list", 2: "line_list", 3: "line_strip", 4: "triangle_list",
              5: "triangle_fan", 6: "triangle_strip", 7: "triangle_with_w_flags", 8: "rectangle_list",
              12: "line_loop", 13: "quad_list", 14: "quad_strip", 15: "polygon"}
COUNTERS = ("draw_updates", "rasterization_updates", "stencil_enabled_updates", "no_read_no_write_updates",
            "empty_scissor_updates", "copy_batches", "copy_commands", "issued_s8_payload_bytes",
            "superseded_before_consumer_bytes", "superseded_cross_frame_bytes",
            "clear_or_native_overwrite_before_consumer_bytes", "required_materialization_payload_bytes",
            "forced_model_materialization_bytes")
ENDINGS = {"window_end_unresolved", "cache_shutdown_unresolved", "disabled_unresolved", "frame_reset_unresolved"}


def decode_state(row):
    control = row["normalized_depth_control"]
    masks = (row["front_masks"], row["back_masks"])
    back = bool(control & 128)
    combined = masks[0] | (masks[1] if back else 0)
    read_mask, write_mask = (combined >> 8) & 255, (combined >> 16) & 255
    z_enabled, zfunc = bool(control & 2), (control >> 4) & 7
    z_pass, z_fail = not z_enabled or zfunc != 0, z_enabled and zfunc != 7
    independent, writes = True, False
    faces, reachable = [], []
    for face in range(2 if back else 1):
        shift = 20 if face else 8
        func, fail, passed, zfailed = ((control >> (shift + n)) & 7 for n in (0, 3, 6, 9))
        stencil_pass, stencil_fail = True, True
        if func == 0:
            stencil_pass = False
        elif func == 7:
            stencil_fail = False
        elif not read_mask:
            stencil_pass = func in (2, 3, 6)
            stencil_fail = not stencil_pass
        applicable = ([fail] if stencil_fail else []) + ([passed] if stencil_pass and z_pass else []) + ([zfailed] if stencil_pass and z_fail else [])
        independent &= func in (0, 7) or read_mask == 0
        writes |= write_mask != 0 and any(op != 0 for op in applicable)
        reachable.extend(applicable)
        faces.append({"face": "back" if face else "front", "func": COMPARE[func],
                      "fail": OPS[fail], "depth_pass": OPS[passed], "depth_fail": OPS[zfailed],
                      "read_mask": (masks[face] >> 8) & 255, "write_mask": (masks[face] >> 16) & 255,
                      "reachable_ops_conservative": [OPS[op] for op in sorted(set(applicable))]})
    full_masks = all(((masks[i] >> 16) & 255) == 255 for i in range(2 if back else 1))
    constant_writer = bool(independent and writes and full_masks and all(op in (0, 1, 2) for op in reachable))
    initiator = row.get("draw_initiator")
    primitive = None if initiator is None else {"raw": initiator, "type": initiator & 63,
                 "name": PRIMITIVES.get(initiator & 63, "other_or_tessellated"), "index_count": initiator >> 16,
                 "source_select": (initiator >> 6) & 3, "index_size_bit": (initiator >> 11) & 1}
    return {"depth_enabled": z_enabled, "depth_func": COMPARE[zfunc], "backface_enabled": back, "draw": primitive,
            "faces": faces, "comparison_independent": bool(independent), "writes_possible": bool(writes),
            "no_read_no_write": bool(independent and not writes),
            "constant_full_mask_writer_needs_coverage_proof": constant_writer,
            "safe_full_resource_cancellation": False,
            "classification_matches_observer": bool(independent) == row["comparison_independent"] and bool(writes) == row["writes_possible"]}


def load_records(path):
    marker = "Stencil consumer survey "
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line_number, line in enumerate(stream, 1):
            position = line.find(marker + "{")
            if position < 0:
                continue
            try:
                row = json.loads(line[position + len(marker):])
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number}: malformed survey JSON: {exc}") from exc
            row["source_line"] = line_number
            yield row


def window_delta(begin, end):
    delta = {name: end.get(name, 0) - begin.get(name, 0) for name in COUNTERS}
    if any(value < 0 for value in delta.values()):
        raise ValueError("Survey counters went backwards within one owner/session")
    frames = end["frame"] - begin["frame"]
    start_pending, end_pending = begin.get("pending_unresolved_bytes", 0), end.get("pending_unresolved_bytes", 0)
    available = delta["issued_s8_payload_bytes"] + start_pending
    avoided = delta["superseded_before_consumer_bytes"] + delta["clear_or_native_overwrite_before_consumer_bytes"]
    accounted = avoided + delta["required_materialization_payload_bytes"] + delta["forced_model_materialization_bytes"] + end_pending
    reasons = {}
    for name, reason in end.get("reasons", {}).items():
        old = begin.get("reasons", {}).get(name, {})
        reasons[name] = {key: reason.get(key, 0) - old.get(key, 0)
                         for key in ("events", "materialization_events", "payload_bytes", "cross_frame_payload_bytes")}
    return {"begin_frame": begin["frame"], "end_frame_exclusive": end["frame"], "frames": frames,
            "counters": delta, "begin_pending_bytes": start_pending, "end_pending_bytes": end_pending,
            "available_payload_bytes_including_carry_in": available,
            "candidate_avoidable_payload_bytes": avoided,
            "candidate_avoidable_share_of_available_payload": avoided / available if available else 0,
            "issued_payload_bytes_per_frame": delta["issued_s8_payload_bytes"] / frames if frames else None,
            "candidate_avoidable_payload_bytes_per_frame": avoided / frames if frames else None,
            "byte_accounting_balanced": available == accounted, "reasons": reasons}


def summarize_session(path, records, steady_skip):
    records.sort(key=lambda row: (row["frame"], row["source_line"]))
    final = records[-1]
    zero = {"frame": final["start_frame"], "pending_unresolved_bytes": 0, "reasons": {}}
    complete = final["boundary"] in ENDINGS
    all_balanced = all(row.get("byte_accounting_balanced", False) for row in records)
    no_gaps = all(row.get("unidentified_resource_events", 0) == 0 for row in records)
    full = window_delta(zero, final)
    choices = [row for row in records if row["frame"] >= final["start_frame"] + steady_skip and row["frame"] < final["frame"]]
    steady = window_delta(choices[0], final) if choices else None
    resources, mismatches, constant_writers = [], 0, []
    for resource in final.get("resources", []):
        item = dict(resource)
        states = []
        for row in resource.get("draw_states", []):
            decoded = decode_state(row)
            mismatches += not decoded["classification_matches_observer"]
            draws, samples = row["draws"], resource["samples"]
            area = resource["width"] * resource["height"] * samples
            states.append({**row, "decoded": decoded,
                           "average_scissor_resource_area_fraction": row["scissor_sample_area_sum"] / (draws * area) if draws and area else None})
            if decoded["constant_full_mask_writer_needs_coverage_proof"]:
                constant_writers.append({"resource_id": resource["id"], "normalized_depth_control": row["normalized_depth_control"],
                                         "draws": draws, "materialized_payload_bytes": row["materialized_payload_bytes"],
                                         "scissor_union": row["scissor_union"], "vertex_shader_hash": row.get("vertex_shader_hash"),
                                         "active_pixel_shader_hash": row.get("pixel_shader_hash"), "draw": decoded["draw"],
                                         "active_pixel_shader_kills": row.get("pixel_kills"),
                                         "active_pixel_shader_writes_depth": row.get("pixel_writes_depth"),
                                         "active_pixel_shader_may_be_stale_or_unbound": True, "safe_to_cancel": False})
        item["draw_states"] = sorted(states, key=lambda row: row["materialized_payload_bytes"], reverse=True)
        resources.append(item)
    usable = complete and all_balanced and full["byte_accounting_balanced"] and no_gaps and not mismatches
    target = steady if steady and steady["frames"] else full
    if not usable:
        decision = "capture_incomplete_or_invalid_do_not_estimate_savings"
    elif target["candidate_avoidable_payload_bytes"]:
        decision = "unused_pending_payload_observed_snapshot_safe_prototype_and_real_ab_required"
    else:
        decision = "no_unused_payload_observed_do_not_skip_or_defer_copies_based_on_counts_alone"
    return {"log": str(path.resolve()), "session": final["session"], "owner": final["owner"],
            "schema": "state_scissor" if "no_read_no_write_updates" in final else "whole_resource_conservative",
            "final_boundary": final["boundary"], "terminal_record_seen": complete,
            "observation_usable": usable, "unidentified_resource_events": final.get("unidentified_resource_events", 0),
            "histogram_complete": final.get("histogram_overflow_updates", 0) == 0,
            "decoded_classification_mismatches": mismatches,
            "whole_window": full, "steady_window": steady,
            "stencil_enabled_update_fraction": final["stencil_enabled_updates"] / final["draw_updates"] if final["draw_updates"] else 0,
            "no_read_no_write_fraction_of_stencil_enabled": final.get("no_read_no_write_updates", 0) / final["stencil_enabled_updates"] if final["stencil_enabled_updates"] else 0,
            "resources": sorted(resources, key=lambda row: row["issued_bytes"], reverse=True),
            "constant_full_mask_writer_states_needing_coverage_proof": sorted(constant_writers, key=lambda row: row["materialized_payload_bytes"], reverse=True),
            "decision": decision,
            "requirements_before_rendering_changes": [
                "Keep per-destination and per-region snapshot identities, lifetime and GPU fences; current shared R8 scratch may be overwritten.",
                "Materialize needed pending regions before dependent reads and before writes with unknown coverage or partial masks; remove those regions so later snapshots cannot overwrite newer stencil writes.",
                "ALWAYS/REPLACE and full write masks do not establish full geometry/depth/discard coverage. No full-resource clear cancellation is inferred.",
                "Pending bytes at session end have unknown future consumers and are not savings. Slice accounting includes pending carry-in.",
                "Logical payload reduction is not an FPS result. Run exact native GPU differential/lifetime checks and matching-feature steady-scene A/B."]}


def analyze(paths, steady_skip):
    sessions = []
    for path in paths:
        groups = defaultdict(list)
        for record in load_records(path):
            groups[(record["owner"], record["session"], record["start_frame"])].append(record)
        for records in groups.values():
            sessions.append(summarize_session(path, records, steady_skip))
    return {"scope": "CPU-only S8 lifetime and conservative state/scissor model; rendering unchanged, no measured FPS or VRAM-bandwidth claim.",
            "steady_skip_frames": steady_skip, "sessions": sessions}


def self_test():
    always = 1 | (7 << 8)
    row = {"normalized_depth_control": always, "front_masks": 0xFFFF00, "back_masks": 0xFFFF00,
           "comparison_independent": True, "writes_possible": False}
    assert decode_state(row)["no_read_no_write"]
    writer = {**row, "normalized_depth_control": always | (2 << 14), "writes_possible": True}
    assert decode_state(writer)["constant_full_mask_writer_needs_coverage_proof"]
    assert not decode_state(writer)["safe_full_resource_cancellation"]
    different_back = {**row, "normalized_depth_control": always | 128 | (2 << 20), "back_masks": 0,
                      "comparison_independent": False, "writes_possible": False}
    assert not decode_state(different_back)["no_read_no_write"]
    begin = {"frame": 10, "issued_s8_payload_bytes": 1000, "superseded_before_consumer_bytes": 100,
             "clear_or_native_overwrite_before_consumer_bytes": 100, "required_materialization_payload_bytes": 600,
             "pending_unresolved_bytes": 200}
    end = {"frame": 20, "issued_s8_payload_bytes": 2000, "superseded_before_consumer_bytes": 200,
           "clear_or_native_overwrite_before_consumer_bytes": 100, "required_materialization_payload_bytes": 1600,
           "pending_unresolved_bytes": 100}
    delta = window_delta(begin, end)
    assert delta["byte_accounting_balanced"] and delta["available_payload_bytes_including_carry_in"] == 1200
    assert delta["candidate_avoidable_payload_bytes"] == 100 and delta["begin_pending_bytes"] == 200
    print("Survey parser: raw-state classification, unsafe constant-writer exclusion and exact carry-in byte accounting passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="*", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--steady-skip-frames", type=int, default=300)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if not args.logs:
        if not args.self_test:
            parser.error("Provide at least one captured game.log")
        return
    result = analyze(args.logs, max(0, args.steady_skip_frames))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for session in result["sessions"]:
        window = session["steady_window"] or session["whole_window"]
        print(json.dumps({"log": session["log"], "session": session["session"], "schema": session["schema"],
                          "usable": session["observation_usable"], "frames": window["frames"],
                          "issued_s8_payload_bytes_per_frame": window["issued_payload_bytes_per_frame"],
                          "candidate_avoidable_payload_bytes_per_frame": window["candidate_avoidable_payload_bytes_per_frame"],
                          "no_read_no_write_fraction": session["no_read_no_write_fraction_of_stencil_enabled"],
                          "decision": session["decision"]}))


if __name__ == "__main__":
    main()
