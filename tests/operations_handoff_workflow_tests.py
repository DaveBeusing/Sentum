#!/usr/bin/env python3
# Copyright (C) 2026 Dave Beusing <david.beusing@gmail.com>
# SPDX-License-Identifier: MIT

from __future__ import annotations

import copy
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PRODUCER = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8')
CONSUMER = (ROOT / '.github/workflows/production-operations.yml').read_text(encoding='utf-8')


def resolve(value: str, context: dict):
    value = value.strip()
    if re.fullmatch(r"'[^']*'", value):
        return value[1:-1]
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.]*', value):
        raise AssertionError(f'Unsupported workflow expression: {value}')
    result = context
    for part in value.split('.'):
        if not isinstance(result, dict):
            return None
        result = result.get(part)
    return result


def selected(context: dict) -> bool:
    # Evaluate the equality/conjunction expression read from the workflow itself.
    match = re.search(r'(?ms)^    if: (?:>-\s*)?\$\{\{(.*?)\}\}', CONSUMER)
    if match is None:
        raise AssertionError('Missing operations source condition')
    for clause in match.group(1).split('&&'):
        operands = clause.strip().split('==')
        if len(operands) != 2:
            raise AssertionError(f'Unsupported workflow comparison: {clause}')
        left, right = (resolve(value, context) for value in operands)
        if left is None or right is None or left != right:
            return False
    return True


def rendered_field(workflow: str, step_name: str, field: str, context: dict) -> str:
    step = workflow.split(f'      - name: {step_name}\n', 1)[1].split('\n      - ', 1)[0]
    match = re.search(rf'(?m)^\s+{re.escape(field)}: (.+)$', step)
    if match is None:
        raise AssertionError(f'Missing {field} in {step_name}')
    def replacement(value: re.Match) -> str:
        result = resolve(value.group(1), context)
        if result is None:
            raise AssertionError('Unresolved artifact identity')
        return str(result)
    return re.sub(r'\$\{\{(.*?)\}\}', replacement, match.group(1))


class OperationsHandoffWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        # Observed successful master producer and its exact published RC artifact.
        self.sha = '09958b5ebf0f8afcdf240047f123e398c267436d'
        self.context = {'github': {
            'repository': 'DaveBeusing/Sentum',
            'sha': 'downstream-workflow-sha-must-not-be-used',
            'event': {'workflow_run': {
                'conclusion': 'success', 'event': 'push', 'head_branch': 'master',
                'head_sha': self.sha, 'id': 40137257401,
                'head_repository': {'full_name': 'DaveBeusing/Sentum'},
            }},
        }}

    def test_integrated_push_uses_produced_commit_and_exact_source_run(self) -> None:
        self.assertTrue(selected(self.context))
        producer_context = {'github': {'sha': self.sha}}
        produced = rendered_field(PRODUCER, 'Upload release candidate handoff', 'name', producer_context)
        requested = rendered_field(CONSUMER, 'Download RC handoff artifact', 'name', self.context)
        self.assertEqual(f'sentum-rc-{self.sha}', produced)
        self.assertEqual(produced, requested)
        self.assertEqual(self.sha, rendered_field(CONSUMER, 'Checkout evaluated commit', 'ref', self.context))
        self.assertEqual('40137257401', rendered_field(
            CONSUMER, 'Download RC handoff artifact', 'run-id', self.context))
        for step in ('Validate RC handoff evidence', 'Enforce production operations baseline',
                     'Run operational validation rehearsals', 'Run continuous readiness rehearsal',
                     'Enforce reliability policy', 'Enforce resilience recovery guardrails',
                     'Enforce operations control-plane governance', 'Consolidate readiness evidence'):
            self.assertEqual(self.sha, rendered_field(CONSUMER, step, 'GITHUB_SHA', self.context))

    def test_observed_pr_merge_head_mismatch_is_skipped(self) -> None:
        source = self.context['github']['event']['workflow_run']
        source.update(event='pull_request', head_branch='fix/runtime-lifecycle-finalization',
                      head_sha='6eebec90469883f1d98e631df5ca3b3978d1b630', id=39983015881)
        merge_sha = 'e7fe2e839c2ba317eb35a1fefc496d620dc7d6b6'
        produced = rendered_field(PRODUCER, 'Upload release candidate handoff', 'name',
                                  {'github': {'sha': merge_sha}})
        self.assertEqual(f'sentum-rc-{merge_sha}', produced)
        self.assertNotEqual(produced, rendered_field(CONSUMER, 'Download RC handoff artifact',
                                                     'name', self.context))
        self.assertFalse(selected(self.context))
        source['head_branch'] = 'master'
        self.assertFalse(selected(self.context), 'PRs from a branch named master are still PRs')

    def test_other_events_branches_repositories_and_results_are_skipped(self) -> None:
        for field, invalid_values in {
            'event': ['pull_request', 'schedule', 'workflow_dispatch', 'merge_group', None],
            'head_branch': ['main', 'fix/feature', 'release/1.0', None],
            'conclusion': ['failure', 'cancelled', 'timed_out', 'skipped', None],
            'head_repository': [{'full_name': 'elsewhere/Sentum'}, None],
        }.items():
            for value in invalid_values:
                with self.subTest(field=field, value=value):
                    context = copy.deepcopy(self.context)
                    context['github']['event']['workflow_run'][field] = value
                    self.assertFalse(selected(context))

    def test_source_run_binding_is_preserved_by_downstream_gates(self) -> None:
        for step in ('Validate RC handoff evidence', 'Enforce production operations baseline',
                     'Run operational validation rehearsals', 'Run continuous readiness rehearsal',
                     'Enforce reliability policy', 'Enforce resilience recovery guardrails',
                     'Enforce operations control-plane governance', 'Consolidate readiness evidence'):
            self.assertEqual('40137257401', rendered_field(
                CONSUMER, step, 'SOURCE_WORKFLOW_RUN_ID', self.context))


if __name__ == '__main__':
    unittest.main()
