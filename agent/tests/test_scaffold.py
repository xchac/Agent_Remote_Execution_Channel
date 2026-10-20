import unittest

import agent.main


class ScaffoldTest(unittest.TestCase):
    def test_main_imports(self) -> None:
        self.assertEqual(agent.main.main(), 0)


if __name__ == "__main__":
    unittest.main()

