import subprocess
import pytest
import textwrap

@pytest.mark.parametrize(
    "a, b",
    [
        [1, 3],
        [23, -23],
        [3.14, -3.14],
        [0, 100],
        [-100, 5],
    ]
)
def test_assignment(a, b):
    source = textwrap.dedent(
        f"""\
        let x = {a};
        print x;
        x = {b};
        print x;"""
    )
    
    process = subprocess.run([
        "valgrind",
        "--leak-check=full",
        "--show-leak-kinds=all",
        "--error-exitcode=1",
        "./a.out"],
        capture_output=True,
        input=source.encode('utf-8')
    )
    for value in [a, b]:
        assert f"dbg print :: {value:.2f}\n".encode('utf-8') in process.stdout
        assert process.returncode == 0