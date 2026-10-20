import unittest

import agent.main


class ScaffoldTest(unittest.TestCase):
    def test_main_imports(self) -> None:
        self.assertTrue(callable(agent.main.main))


if __name__ == "__main__":
    unittest.main()

