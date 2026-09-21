#!/usr/bin/env python3
"""Replay a Box co-write trace under alternative stable variable groupings."""

import argparse
import collections
import heapq
import json
import math
from pathlib import Path


CREATE, COPY_CONSTRUCT, MOVE_CONSTRUCT, COPY_ASSIGN, MOVE_ASSIGN, DESTROY = range(6)
JOIN = 9
EXPLICIT_PAIR_LIMIT = 32


def variable(text):
    sign = text[0] if text[0] in "+-" else ""
    fields = text[len(sign):].split(":")
    if len(fields) != 4:
        raise ValueError(f"invalid variable token: {text}")
    return sign, tuple(map(int, fields))


def read_trace(path):
    events = []
    variables = set()
    marginal = collections.Counter()
    pair = collections.Counter()
    hyperedges = collections.Counter()
    raw = {"detaches": 0, "cloned_slots": 0}
    raw_epochs = collections.defaultdict(collections.Counter)
    lines = path.read_text().splitlines()
    if any(line == "# box-cowrite-trace-v1" for line in lines):
        raise ValueError(
            "box-cowrite-trace-v1 conflates Bottom snapshots with actual touches")
    for line_number, line in enumerate(lines, 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        tag = fields[0]
        if tag == "S":
            if len(fields) != 7:
                raise ValueError(f"line {line_number}: invalid S record")
            events.append(("S",) + tuple(map(int, fields[1:])))
        elif tag == "M":
            if "T" not in fields:
                raise ValueError(f"line {line_number}: missing touched delimiter")
            delimiter = fields.index("T")
            head = list(map(int, fields[1:9]))
            changed_count = head[-1]
            changed_tokens = fields[9:delimiter]
            if len(changed_tokens) != changed_count:
                raise ValueError(f"line {line_number}: changed count mismatch")
            touched_count = int(fields[delimiter + 1])
            touched_tokens = fields[delimiter + 2:]
            if len(touched_tokens) != touched_count:
                raise ValueError(f"line {line_number}: touched count mismatch")
            changed = []
            for token in changed_tokens:
                sign, key = variable(token)
                changed.append((key, sign == "+"))
                variables.add(key)
            changed_keys = [key for key, _ in changed]
            # Transitioning to Bottom drops the directory as a whole.  The
            # changed support is semantic, not a set of per-variable page
            # writes, so it must not create a layout affinity hyperedge.
            if not head[6]:
                if len(changed_keys) <= EXPLICIT_PAIR_LIMIT:
                    for index, left in enumerate(changed_keys):
                        for right in changed_keys[index + 1:]:
                            pair[tuple(sorted((left, right)))] += 1
                else:
                    hyperedges[tuple(sorted(changed_keys))] += 1
            touched = []
            for token in touched_tokens:
                sign, key = variable(token)
                if sign:
                    raise ValueError(f"line {line_number}: signed touched variable")
                touched.append(key)
                variables.add(key)
                marginal[key] += 1
            events.append(("M", *head[:-1], changed, touched))
        elif tag == "D":
            raw["detaches"] += 1
            raw_epochs[int(fields[2])]["detaches"] += 1
        elif tag == "W":
            raw["cloned_slots"] += int(fields[3])
            raw_epochs[int(fields[1])]["cloned_slots"] += int(fields[3])
        else:
            raise ValueError(f"line {line_number}: unknown record {tag}")
    return events, variables, marginal, pair, hyperedges, raw, raw_epochs


def current_mapping(variables):
    return {key: key[0] // 8 for key in variables}


def marginal_mapping(variables, marginal):
    ordered = sorted(variables, key=lambda key: (-marginal[key], key))
    return {key: index // 8 for index, key in enumerate(ordered)}


def relational_mapping(variables, marginal, pair, hyperedges=()):
    ordered = sorted(variables, key=lambda key: (-marginal[key], key))
    neighbours = collections.defaultdict(list)
    for (left, right), weight in pair.items():
        if weight <= 0:
            continue
        neighbours[left].append((right, weight))
        neighbours[right].append((left, weight))
    memberships = collections.defaultdict(list)
    edge_groups = []
    items = hyperedges.items() if hasattr(hyperedges, "items") else hyperedges
    for members, weight in items:
        if weight <= 0:
            continue
        edge_index = len(edge_groups)
        edge_groups.append(collections.Counter())
        for key in members:
            memberships[key].append((edge_index, weight))
    groups = []
    open_groups = []
    mapping = {}
    for key in ordered:
        affinity = collections.Counter()
        for other, weight in neighbours[key]:
            if other in mapping:
                group_index = mapping[other]
                if len(groups[group_index]) < 8:
                    affinity[group_index] += weight
        # A large touched set denotes a uniformly weighted clique.  Keeping it
        # as a hyperedge avoids materializing O(k^2) pairs while preserving the
        # exact affinity: each already assigned member in a candidate group
        # contributes the hyperedge weight once.
        for edge_index, weight in memberships[key]:
            for group_index, count in edge_groups[edge_index].items():
                if len(groups[group_index]) < 8:
                    affinity[group_index] += weight * count
        if affinity:
            group_index = max(
                (weight, -len(groups[index]), -index, index)
                for index, weight in affinity.items()
            )[-1]
        else:
            while open_groups and len(groups[open_groups[0]]) == 8:
                heapq.heappop(open_groups)
            if open_groups:
                group_index = open_groups[0]
            else:
                group_index = len(groups)
                groups.append([])
                heapq.heappush(open_groups, group_index)
        groups[group_index].append(key)
        mapping[key] = group_index
        for edge_index, _ in memberships[key]:
            edge_groups[edge_index][group_index] += 1
    return mapping


def clone_conflict_mapping(variables, marginal, conflicts):
    """Pack variables while avoiding pairs that caused cloned-slot work.

    The number of groups is fixed to ceil(|variables| / 8), so this control
    cannot buy fewer cloned slots by creating an unbounded sparse directory.
    """
    if not conflicts:
        return marginal_mapping(variables, marginal)
    weighted_degree = collections.Counter()
    neighbours = collections.defaultdict(list)
    for (left, right), weight in conflicts.items():
        if weight <= 0:
            continue
        weighted_degree[left] += weight
        weighted_degree[right] += weight
        neighbours[left].append((right, weight))
        neighbours[right].append((left, weight))
    ordered = sorted(
        variables,
        key=lambda key: (-weighted_degree[key], -marginal[key], key),
    )
    groups = [[] for _ in range(math.ceil(len(ordered) / 8))]
    # A full scan of all groups for every variable is quadratic when the trace
    # contains many variables but only a sparse conflict graph.  Keep one heap
    # per occupancy instead.  For a variable, only groups containing an actual
    # conflict neighbour need to be skipped; stale heap entries are discarded
    # lazily after a group changes size.
    open_by_size = [[] for _ in range(8)]
    open_by_size[0] = list(range(len(groups)))
    heapq.heapify(open_by_size[0])
    mapping = {}
    for key in ordered:
        conflict_cost = collections.Counter()
        for other, weight in neighbours[key]:
            if other in mapping:
                conflict_cost[mapping[other]] += weight
        group_index = None
        for occupancy in range(7, -1, -1):
            candidates = open_by_size[occupancy]
            skipped = []
            while candidates:
                candidate = candidates[0]
                if len(groups[candidate]) != occupancy:
                    heapq.heappop(candidates)
                elif conflict_cost[candidate] > 0:
                    skipped.append(heapq.heappop(candidates))
                else:
                    group_index = candidate
                    break
            for candidate in skipped:
                heapq.heappush(candidates, candidate)
            if group_index is not None:
                break
        if group_index is None:
            # Every open group conflicts.  This is possible only for a dense
            # conflict neighbourhood; choose the least costly open group and
            # preserve the same denser-group/deterministic tie break.
            group_index = min(
                (cost, -len(groups[index]), index)
                for index, cost in conflict_cost.items()
                if len(groups[index]) < 8)[-1]
        groups[group_index].append(key)
        mapping[key] = group_index
        new_size = len(groups[group_index])
        if new_size < 8:
            heapq.heappush(open_by_size[new_size], group_index)
    return mapping


def mapping_signature(mapping):
    return tuple(sorted(mapping.items()))


class Replay:
    def __init__(self, mapping, collect_clone_conflicts=False):
        self.mapping = mapping
        self.collect_clone_conflicts = collect_clone_conflicts
        self.clone_conflicts = collections.Counter()
        self.clone_triggers = collections.Counter()
        self.states = {}
        self.pages = {}
        self.references = collections.Counter()
        self.next_page = 1
        self.replacement_epochs = set()
        self.metrics = collections.Counter()
        self.diagnostics = {"mismatch_count": 0, "first_mismatches": []}

    def retain(self, page):
        self.references[page] += 1

    def release(self, page):
        self.references[page] -= 1
        if self.references[page] == 0:
            del self.references[page]
            del self.pages[page]

    def clear(self, state):
        for page in state["pages"].values():
            self.release(page)
        state["pages"].clear()

    def fresh_page(self, values=()):
        page = self.next_page
        self.next_page += 1
        self.pages[page] = set(values)
        self.references[page] = 0
        self.metrics["page_allocations"] += 1
        return page

    def share_from(self, destination, source):
        self.clear(destination)
        destination["pages"] = dict(source["pages"])
        for page in destination["pages"].values():
            self.retain(page)
        destination["bottom"] = source["bottom"]

    def move_from(self, destination, source):
        self.clear(destination)
        destination["pages"] = source["pages"]
        destination["bottom"] = source["bottom"]
        source["pages"] = {}

    def lifecycle(self, event):
        _, sequence, kind, state_id, source_id, epoch, bottom = event
        del sequence
        if kind == CREATE:
            if state_id in self.states:
                raise ValueError(f"duplicate state {state_id}")
            self.states[state_id] = {"pages": {}, "bottom": bool(bottom)}
            return
        if kind == DESTROY:
            state = self.states.pop(state_id)
            self.clear(state)
            return
        source = self.states[source_id]
        if kind in (COPY_CONSTRUCT, MOVE_CONSTRUCT):
            if state_id in self.states:
                raise ValueError(f"duplicate constructed state {state_id}")
            self.states[state_id] = {"pages": {}, "bottom": bool(bottom)}
        destination = self.states[state_id]
        if kind in (COPY_CONSTRUCT, COPY_ASSIGN):
            self.share_from(destination, source)
        elif kind in (MOVE_CONSTRUCT, MOVE_ASSIGN):
            self.move_from(destination, source)
        else:
            raise ValueError(f"invalid lifecycle kind {kind}")
        if epoch:
            self.replacement_epochs.add(epoch)

    def detach(self, state, page_index, trigger=None):
        old = state["pages"][page_index]
        if self.references[old] == 1:
            return old
        values = self.pages[old]
        if self.collect_clone_conflicts and trigger is not None:
            self.clone_triggers[trigger] += 1
            for other in values:
                if other != trigger:
                    self.clone_conflicts[tuple(sorted((trigger, other)))] += 1
        new = self.fresh_page(values)
        self.metrics["detaches"] += 1
        self.metrics["cloned_slots"] += len(values)
        self.release(old)
        self.retain(new)
        state["pages"][page_index] = new
        return new

    def apply_support(self, state, changed):
        for key, present in changed:
            page_index = self.mapping[key]
            page = state["pages"].get(page_index)
            if present:
                if page is None:
                    page = self.fresh_page()
                    self.retain(page)
                    state["pages"][page_index] = page
                self.pages[page].add(key)
            elif page is not None:
                self.pages[page].discard(key)
                if not self.pages[page]:
                    self.release(page)
                    del state["pages"][page_index]

    def join(self, state, related, changed):
        for page_index, left_page in list(state["pages"].items()):
            right_page = related["pages"].get(page_index)
            self.metrics["page_touches"] += 1
            self.metrics["directory_chunks_touched"] += 1
            if right_page is None:
                self.release(left_page)
                del state["pages"][page_index]
            elif left_page != right_page:
                values = self.pages[left_page]
                new_page = self.fresh_page(values)
                self.metrics["detaches"] += 1
                self.metrics["cloned_slots"] += len(values)
                self.release(left_page)
                self.retain(new_page)
                state["pages"][page_index] = new_page
        self.apply_support(state, changed)

    def mutation(self, event):
        _, sequence, epoch, kind, state_id, related_id, before_bottom, after_bottom, changed, touched = event
        del sequence, before_bottom
        state = self.states[state_id]
        if epoch in self.replacement_epochs:
            self.replacement_epochs.remove(epoch)
            state["bottom"] = bool(after_bottom)
            return
        if kind == JOIN and not state["bottom"] and related_id:
            related = self.states[related_id]
            if related["bottom"]:
                state["bottom"] = bool(after_bottom)
                return
            self.join(state, related, changed)
            state["bottom"] = bool(after_bottom)
            return
        changed_map = dict(changed)
        touched_pages = set()
        for key in touched:
            page_index = self.mapping[key]
            page = state["pages"].get(page_index)
            present = changed_map.get(key)
            if page is None and present is not True:
                continue
            # Missing slots denote Top. A Top-to-Top update reaches
            # eraseBound(), which returns before requesting a writable page;
            # sharing unrelated slots in that page therefore causes no detach.
            if (page is not None and present is None and
                    key not in self.pages[page]):
                continue
            if page_index not in touched_pages:
                self.metrics["page_touches"] += 1
                self.metrics["directory_chunks_touched"] += 1
                touched_pages.add(page_index)
            if page is not None:
                self.detach(state, page_index, key)
        if after_bottom:
            self.clear(state)
            state["bottom"] = True
            return
        state["bottom"] = False
        self.apply_support(state, changed)

    def run(self, events, raw_epochs=None):
        seen_epochs = set()
        for event in events:
            before_detaches = self.metrics["detaches"]
            before_slots = self.metrics["cloned_slots"]
            if event[0] == "S":
                self.lifecycle(event)
            else:
                self.mutation(event)
                if raw_epochs is not None:
                    epoch = event[2]
                    seen_epochs.add(epoch)
                    expected = raw_epochs.get(epoch, {})
                    actual_detaches = self.metrics["detaches"] - before_detaches
                    actual_slots = self.metrics["cloned_slots"] - before_slots
                    expected_detaches = expected.get("detaches", 0)
                    expected_slots = expected.get("cloned_slots", 0)
                    if (actual_detaches != expected_detaches or
                            actual_slots != expected_slots):
                        self.diagnostics["mismatch_count"] += 1
                        if len(self.diagnostics["first_mismatches"]) < 32:
                            self.diagnostics["first_mismatches"].append({
                                "epoch": epoch,
                                "kind": event[3],
                                "state": event[4],
                                "related_state": event[5],
                                "before_bottom": event[6],
                                "after_bottom": event[7],
                                "changed": len(event[8]),
                                "touched": len(event[9]),
                                "expected_detaches": expected_detaches,
                                "actual_detaches": actual_detaches,
                                "expected_cloned_slots": expected_slots,
                                "actual_cloned_slots": actual_slots,
                            })
        if raw_epochs is not None:
            for epoch in sorted(set(raw_epochs) - seen_epochs):
                expected = raw_epochs[epoch]
                self.diagnostics["mismatch_count"] += 1
                if len(self.diagnostics["first_mismatches"]) < 32:
                    self.diagnostics["first_mismatches"].append({
                        "epoch": epoch,
                        "missing_mutation_record": True,
                        "expected_detaches": expected.get("detaches", 0),
                        "expected_cloned_slots": expected.get("cloned_slots", 0),
                    })
        self.metrics["live_states"] = len(self.states)
        self.metrics["live_pages"] = len(self.pages)
        return dict(self.metrics)


def future_conflict_search(events, variables, marginal, initial_mapping,
                           rounds):
    """Use the full future trace to iteratively expose clone conflicts.

    This is a feasible future-informed search, not a proof of the global
    optimum.  It supplies a stronger G4 candidate while the separate optimistic
    bound remains responsible for ruling out headroom.
    """
    mapping = initial_mapping
    cumulative = collections.Counter()
    seen = set()
    candidates = []
    for round_index in range(rounds):
        signature = mapping_signature(mapping)
        if signature in seen:
            break
        seen.add(signature)
        engine = Replay(mapping, collect_clone_conflicts=True)
        metrics = engine.run(events)
        candidates.append({
            "round": round_index,
            "mapping": mapping,
            "metrics": metrics,
            "conflict_pairs": len(engine.clone_conflicts),
            "conflict_weight": sum(engine.clone_conflicts.values()),
        })
        cumulative.update(engine.clone_conflicts)
        next_mapping = clone_conflict_mapping(
            variables, marginal, cumulative)
        if mapping_signature(next_mapping) == signature:
            break
        mapping = next_mapping
    if not candidates:
        raise ValueError("future conflict search requires at least one round")
    objective = lambda item: (
        item["metrics"].get("cloned_slots", 0),
        item["metrics"].get("detaches", 0),
        item["metrics"].get("page_touches", 0),
        item["metrics"].get("page_allocations", 0),
        item["round"],
    )
    best = min(candidates, key=objective)
    return best["mapping"], {
        "rounds_evaluated": len(candidates),
        "best_round": best["round"],
        "candidates": [{key: value for key, value in item.items()
                        if key != "mapping"} for item in candidates],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--g0-only", action="store_true")
    parser.add_argument("--future-rounds", type=int, default=0)
    args = parser.parse_args()
    if args.future_rounds < 0:
        parser.error("--future-rounds must be non-negative")
    events, variables, marginal, pair, hyperedges, raw, raw_epochs = read_trace(args.trace)
    g0_mapping = current_mapping(variables)
    mappings = {"g0_current": g0_mapping}
    g0_engine = Replay(g0_mapping, collect_clone_conflicts=True)
    replay = {
        "g0_current": g0_engine.run(events, raw_epochs),
    }
    g0_diagnostics = g0_engine.diagnostics
    future_search = None
    if not args.g0_only:
        mappings.update({
            "g2_marginal": marginal_mapping(variables, marginal),
            "g3_cowrite": relational_mapping(
                variables, marginal, pair, hyperedges),
            "g3_clone_conflict": clone_conflict_mapping(
                variables, marginal, g0_engine.clone_conflicts),
        })
        if args.future_rounds:
            mappings["g4_future_conflict"], future_search = (
                future_conflict_search(
                    events, variables, marginal, g0_mapping,
                    args.future_rounds))
    for name, mapping in mappings.items():
        if name == "g0_current":
            continue
        engine = Replay(mapping)
        replay[name] = engine.run(events)
    validation = {
        "detach_match": replay["g0_current"].get("detaches", 0) == raw["detaches"],
        "cloned_slots_match": replay["g0_current"].get("cloned_slots", 0) == raw["cloned_slots"],
    }
    result = {
        "schema": "box-cowrite-replay-v2",
        "trace": str(args.trace),
        "variables": len(variables),
        "mutation_events": sum(event[0] == "M" for event in events),
        "cowrite_model": {
            "explicit_pairs": len(pair),
            "large_hyperedges": len(hyperedges),
            "explicit_pair_limit": EXPLICIT_PAIR_LIMIT,
        },
        "clone_conflict_model": {
            "pairs": len(g0_engine.clone_conflicts),
            "weight": sum(g0_engine.clone_conflicts.values()),
            "triggers": len(g0_engine.clone_triggers),
            "trigger_events": sum(g0_engine.clone_triggers.values()),
        },
        "future_search": future_search,
        "raw": raw,
        "validation": validation,
        "g0_diagnostics": g0_diagnostics,
        "replay": replay,
    }
    text = json.dumps(result, indent=2, sort_keys=True)
    print(text)
    if args.json:
        args.json.write_text(text + "\n")
    if not all(validation.values()):
        raise SystemExit(2)


if __name__ == "__main__":
    main()
