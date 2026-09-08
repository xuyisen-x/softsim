from invoke import Collection

from . import build, run

ns = Collection()
ns.add_collection(build)
ns.add_collection(run)
