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
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
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
        elif tag == "W":
            raw["cloned_slots"] += int(fields[3])
        else:
            raise ValueError(f"line {line_number}: unknown record {tag}")
    return events, variables, marginal, pair, hyperedges, raw


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


class Replay:
    def __init__(self, mapping):
        self.mapping = mapping
        self.states = {}
        self.pages = {}
        self.references = collections.Counter()
        self.next_page = 1
        self.replacement_epochs = set()
        self.metrics = collections.Counter()

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

    def detach(self, state, page_index):
        old = state["pages"][page_index]
        if self.references[old] == 1:
            return old
        values = self.pages[old]
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
        if after_bottom:
            self.clear(state)
            state["bottom"] = True
            return
        state["bottom"] = False
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
                self.detach(state, page_index)
        self.apply_support(state, changed)

    def run(self, events):
        for event in events:
            if event[0] == "S":
                self.lifecycle(event)
            else:
                self.mutation(event)
        self.metrics["live_states"] = len(self.states)
        self.metrics["live_pages"] = len(self.pages)
        return dict(self.metrics)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    events, variables, marginal, pair, hyperedges, raw = read_trace(args.trace)
    mappings = {
        "g0_current": current_mapping(variables),
        "g2_marginal": marginal_mapping(variables, marginal),
        "g3_cowrite": relational_mapping(variables, marginal, pair, hyperedges),
    }
    replay = {name: Replay(mapping).run(events)
              for name, mapping in mappings.items()}
    validation = {
        "detach_match": replay["g0_current"].get("detaches", 0) == raw["detaches"],
        "cloned_slots_match": replay["g0_current"].get("cloned_slots", 0) == raw["cloned_slots"],
    }
    result = {
        "schema": "box-cowrite-replay-v1",
        "trace": str(args.trace),
        "variables": len(variables),
        "mutation_events": sum(event[0] == "M" for event in events),
        "cowrite_model": {
            "explicit_pairs": len(pair),
            "large_hyperedges": len(hyperedges),
            "explicit_pair_limit": EXPLICIT_PAIR_LIMIT,
        },
        "raw": raw,
        "validation": validation,
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
