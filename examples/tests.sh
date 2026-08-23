# A work list for `shard exec-script tests.sh --distribute`.
#
# One command per line. shard shares the lines out across the machines you
# selected, in proportion to the cpu you gave each of them, and runs each
# machine's share as one script.

pytest tests/unit -k parser
pytest tests/unit -k render
pytest tests/unit -k config
pytest tests/integration -k http
pytest tests/integration -k ssh
pytest tests/slow -k migration
