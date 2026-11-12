"""Sampling limits fail closed; proposal selection cannot silently alter the target."""

import subprocess

import pytest

from test_engine_accuracy_policy import accuracy_launcher


@pytest.mark.parametrize("value", ["0", "65", "100", "-1", "20junk", "0.5", "99999999999999999999", ""])
def test_engine_rejects_unsupported_top_k_before_model_load(accuracy_launcher, value):
    result = subprocess.run([str(accuracy_launcher), "--top-k", value], capture_output=True, text=True)
    assert result.returncode != 0
    assert "top-k" in result.stderr and "1..64" in result.stderr


@pytest.mark.parametrize("value", ["1", "20", "64"])
def test_engine_accepts_exact_supported_top_k(accuracy_launcher, value):
    result = subprocess.run([str(accuracy_launcher), "--top-k", value], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_missing_top_k_argument_is_rejected(accuracy_launcher):
    result = subprocess.run([str(accuracy_launcher), "--top-k"], capture_output=True, text=True)
    assert result.returncode != 0
    assert "top-k" in result.stderr and "1..64" in result.stderr


def test_duplicate_top_k_cannot_hide_unsupported_value(accuracy_launcher):
    result = subprocess.run([str(accuracy_launcher), "--top-k", "0", "--top-k", "20"], capture_output=True, text=True)
    assert result.returncode != 0
    assert "top-k" in result.stderr and "1..64" in result.stderr


def test_confidence_gated_probabilistic_proposals_require_a_different_distribution():
    # Exact two-token counterexample for the experimental q-based rejection mode:
    # p=(.5,.5), q=(.8,.2), confidence floor .3. Only draft A is offered. With
    # probability .8 we accept A with .5/.8, otherwise the residual is B; with
    # probability .2 the omitted draft falls back to a plain p sample. The
    # result is P(A)=.6, not .5. The gate changed q to a point mass but verification
    # used the original q. This is why that experimental mode is disabled; MTP
    # with exact-match target draws remains valid for any chosen draft window.
    p_a, q_a = 0.5, 0.8
    incorrectly_gated_a = q_a * (p_a / q_a) + (1 - q_a) * p_a
    assert incorrectly_gated_a == pytest.approx(0.6)
    assert incorrectly_gated_a != pytest.approx(p_a)
    # Exact-match verification always draws from p, whether a draft was offered.
    exact_match_a = q_a * p_a + (1 - q_a) * p_a
    assert exact_match_a == pytest.approx(p_a)
