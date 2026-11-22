import sys
from setuptools import Extension, setup

setup(
    package_dir={"": "python"},
    packages=["rankpatch"],
    ext_modules=[
        Extension(
            "rankpatch._native",
            sources=["python/rankpatch/_native.cpp"],
            include_dirs=["include"],
            depends=["include/rankpatch.hpp", "include/rankpatch_steady.hpp"],
            language="c++",
            extra_compile_args=["/std:c++17"] if sys.platform == "win32" else ["-std=c++17"],
        )
    ],
)
