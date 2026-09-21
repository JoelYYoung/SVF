#!/usr/bin/env python3
"""Replay a Box co-write trace under alternative stable variable groupings."""

import argparse
import collections
import json
import math
from pathlib import Path


CREATE, COPY_CONSTRUCT, MOVE_CONSTRUCT, COPY_ASSIGN, MOVE_ASSIGN, DESTROY = range(6)
JOIN = 9


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
            touched = []
            for token in touched_tokens:
                sign, key = variable(token)
                if sign:
                    raise ValueError(f"line {line_number}: signed touched variable")
                touched.append(key)
                variables.add(key)
                marginal[key] += 1
            for index, left in enumerate(touched):
                for right in touched[index + 1:]:
                    pair[tuple(sorted((left, right)))] += 1
            events.append(("M", *head[:-1], changed, touched))
        elif tag == "D":
            raw["detaches"] += 1
        elif tag == "W":
            raw["cloned_slots"] += int(fields[3])
        else:
            raise ValueError(f"line {line_number}: unknown record {tag}")
    return events, variables, marginal, pair, raw


def current_mapping(variables):
    return {key: key[0] // 8 for key in variables}


def marginal_mapping(variables, marginal):
    ordered = sorted(variables, key=lambda key: (-marginal[key], key))
    return {key: index // 8 for index, key in enumerate(ordered)}


def relational_mapping(variables, marginal, pair):
    ordered = sorted(variables, key=lambda key: (-marginal[key], key))
    groups = []
    mapping = {}
    for key in ordered:
        candidates = []
        for index, group in enumerate(groups):
            if len(group) == 8:
                continue
            affinity = sum(pair[tuple(sorted((key, other)))] for other in group)
            candidates.append((affinity, -len(group), -index, index))
        if candidates and max(candidates)[0] > 0:
            group_index = max(candidates)[-1]
        else:
            group_index = len(groups)
            groups.append([])
        groups[group_index].append(key)
        mapping[key] = group_index
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
    events, variables, marginal, pair, raw = read_trace(args.trace)
    mappings = {
        "g0_current": current_mapping(variables),
        "g2_marginal": marginal_mapping(variables, marginal),
        "g3_cowrite": relational_mapping(variables, marginal, pair),
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
