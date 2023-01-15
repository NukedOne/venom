import subprocess
import pytest
import textwrap

from tests.util import VALGRIND_CMD
from tests.util import TWO_OPERANDS_GROUP


@pytest.mark.parametrize(
    "a, b",
    TWO_OPERANDS_GROUP,
)
def test_assignment_global(tmp_path, a, b):
    source = textwrap.dedent(
        f"""\
        let x = {a};
        print x;
        x = {b};
        print x;"""
    )

    input_file = tmp_path / "input.vnm"
    input_file.write_text(source)
    
    process = subprocess.run(
        VALGRIND_CMD + [input_file],
        capture_output=True,
    )
    for value in [a, b]:
        assert f"dbg print :: {value:.2f}\n".encode('utf-8') in process.stdout
        assert process.returncode == 0

    # the stack must end up empty
    assert b"stack: []" in process.stdout


@pytest.mark.parametrize(
    "a, b",
    TWO_OPERANDS_GROUP,
)
def test_assignment_func(tmp_path, a, b):
    source = textwrap.dedent(
        """
        fn main() {
            let x = %d;
            print x;
            x = %d;
            print x;
        }
        main();""" % (a, b)
    )

    input_file = tmp_path / "input.vnm"
    input_file.write_text(source)
    
    process = subprocess.run(
        VALGRIND_CMD + [input_file],
        capture_output=True,
    )
    for value in [a, b]:
        assert f"dbg print :: {value:.2f}\n".encode('utf-8') in process.stdout
        assert process.returncode == 0
    
    # the stack must end up empty
    assert b"stack: []" in process.stdout