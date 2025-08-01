from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class GenericUnorderedDataFormatterTestCase(TestBase):
    def setUp(self):
        TestBase.setUp(self)
        self.namespace = "std"

    def do_test_with_run_command(self):
        self.runCmd("file " + self.getBuildArtifact("a.out"), CURRENT_EXECUTABLE_SET)

        lldbutil.run_break_set_by_source_regexp(self, "Set break point at this line.")

        self.runCmd("run", RUN_SUCCEEDED)

        # The stop reason of the thread should be breakpoint.
        self.expect(
            "thread list",
            STOPPED_DUE_TO_BREAKPOINT,
            substrs=["stopped", "stop reason = breakpoint"],
        )

        # This is the function to remove the custom formats in order to have a
        # clean slate for the next test case.
        def cleanup():
            self.runCmd("type format clear", check=False)
            self.runCmd("type summary clear", check=False)
            self.runCmd("type filter clear", check=False)
            self.runCmd("type synth clear", check=False)
            self.runCmd("settings set auto-one-line-summaries true", check=False)

        # Execute the cleanup function during test case tear down.
        self.addTearDownHook(cleanup)

        ns = self.namespace

        # We check here that the map shows 0 children even with corrupt data.
        self.look_for_content_and_continue(
            "corrupt_map", ["%s::unordered_map" % ns, "size=0 {}"]
        )

        # Ensure key/value children, not wrapped in a layer.
        # This regex depends on auto-one-line-summaries.
        self.runCmd("settings set auto-one-line-summaries false")
        children_are_key_value = r"\[0\] = \{\s*first = "

        self.look_for_content_and_continue(
            "map",
            [
                "UnorderedMap",
                children_are_key_value,
                "size=5 {",
                "hello",
                "world",
                "this",
                "is",
                "me",
            ],
        )

        self.look_for_content_and_continue(
            "mmap",
            [
                "UnorderedMultiMap",
                children_are_key_value,
                "size=6 {",
                "first = 3",
                'second = "this"',
                "first = 2",
                'second = "hello"',
            ],
        )

        self.look_for_content_and_continue(
            "iset",
            [
                "IntsUnorderedSet",
                "size=5 {",
                r"\[\d\] = 5",
                r"\[\d\] = 3",
                r"\[\d\] = 2",
            ],
        )

        self.look_for_content_and_continue(
            "sset",
            [
                "StringsUnorderedSet",
                "size=5 {",
                r'\[\d\] = "is"',
                r'\[\d\] = "world"',
                r'\[\d\] = "hello"',
            ],
        )

        self.look_for_content_and_continue(
            "imset",
            [
                "IntsUnorderedMultiSet",
                "size=6 {",
                "(\\[\\d\\] = 3(\\n|.)+){3}",
                r"\[\d\] = 2",
                r"\[\d\] = 1",
            ],
        )

        self.look_for_content_and_continue(
            "smset",
            [
                "StringsUnorderedMultiSet",
                "size=5 {",
                '(\\[\\d\\] = "is"(\\n|.)+){2}',
                '(\\[\\d\\] = "world"(\\n|.)+){2}',
            ],
        )

    def look_for_content_and_continue(self, var_name, patterns):
        self.expect(("frame variable %s" % var_name), ordered=False, patterns=patterns)
        self.expect(("frame variable %s" % var_name), ordered=False, patterns=patterns)
        self.runCmd("continue")

    @add_test_categories(["libstdcxx"])
    def test_with_run_command_libstdcpp(self):
        self.build(dictionary={"USE_LIBSTDCPP": 1})
        self.do_test_with_run_command()

    @add_test_categories(["libstdcxx"])
    def test_with_run_command_libstdcxx_debug(self):
        self.build(
            dictionary={"USE_LIBSTDCPP": 1, "CXXFLAGS_EXTRAS": "-D_GLIBCXX_DEBUG"}
        )
        self.do_test_with_run_command()

    @add_test_categories(["libc++"])
    def test_with_run_command_libcpp(self):
        self.build(dictionary={"USE_LIBCPP": 1})
        self.do_test_with_run_command()

    @add_test_categories(["msvcstl"])
    def test_with_run_command_msvcstl(self):
        # No flags, because the "msvcstl" category checks that the MSVC STL is used by default.
        self.build()
        self.do_test_with_run_command()
