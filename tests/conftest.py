import pytest

import fastpybase64


@pytest.fixture(autouse=True, params=[False, True], ids=["default", "exact-sizing"])
def sizing(request):
    """Runs every test with and without exact output sizing."""
    previous = fastpybase64.get_exact_sizing()
    fastpybase64.set_exact_sizing(request.param)
    yield request.param
    fastpybase64.set_exact_sizing(previous)
