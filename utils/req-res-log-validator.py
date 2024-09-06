#!/usr/bin/env python3
import os
import glob
import json
import sys

import jsonschema
import subprocess
import redis
import time
import argparse
import multiprocessing
import collections
import io
import traceback
from datetime import timedelta
from functools import partial
try:
    from jsonschema import Draft201909Validator as schema_validator
except ImportError:
    from jsonschema import Draft7Validator as schema_validator

"""
The purpose of this file is to validate the reply_schema values of COMMAND DOCS.
Basically, this is what it does:
1. Goes over req-res files, generated by redis-servers, spawned by the testsuite (see logreqres.c)
2. For each request-response pair, it validates the response against the request's reply_schema (obtained from COMMAND DOCS)

This script spins up a redqueue-server and a redqueue-cli in order to obtain COMMAND DOCS.

In order to use this file you must run the redis testsuite with the following flags:
./runtest --dont-clean --force-resp3 --log-req-res

And then:
./utils/req-res-log-validator.py

The script will fail only if:
1. One or more of the replies doesn't comply with its schema.
2. One or more of the commands in COMMANDS DOCS doesn't have the reply_schema field (with --fail-missing-reply-schemas)
3. The testsuite didn't execute all of the commands (with --fail-commands-not-all-hit)

Future validations:
1. Fail the script if one or more of the branches of the reply schema (e.g. oneOf, anyOf) was not hit.
"""

IGNORED_COMMANDS = {
    # Commands that don't work in a req-res manner (see logreqres.c)
    "debug",  # because of DEBUG SEGFAULT
    "sync",
    "psync",
    "monitor",
    "subscribe",
    "unsubscribe",
    "ssubscribe",
    "sunsubscribe",
    "psubscribe",
    "punsubscribe",
    # Commands to which we decided not write a reply schema
    "pfdebug",
    "lolwut",
}

class Request(object):
    """
    This class represents a Redis request (AKA command, argv)
    """
    def __init__(self, f, docs, line_counter):
        """
        Read lines from `f` (generated by logreqres.c) and populates the argv array
        """
        self.command = None
        self.schema = None
        self.argv = []

        while True:
            line = f.readline()
            line_counter[0] += 1
            if not line:
                break
            length = int(line)
            arg = str(f.read(length))
            f.read(2)  # read \r\n
            line_counter[0] += 1
            if arg == "__argv_end__":
                break
            self.argv.append(arg)

        if not self.argv:
            return

        self.command = self.argv[0].lower()
        doc = docs.get(self.command, {})
        if not doc and len(self.argv) > 1:
            self.command = f"{self.argv[0].lower()}|{self.argv[1].lower()}"
            doc = docs.get(self.command, {})

        if not doc:
            self.command = None
            return

        self.schema = doc.get("reply_schema")

    def __str__(self):
        return json.dumps(self.argv)


class Response(object):
    """
    This class represents a Redis response in RESP3
    """
    def __init__(self, f, line_counter):
        """
        Read lines from `f` (generated by logreqres.c) and build the JSON representing the response in RESP3
        """
        self.error = False
        self.queued = False
        self.json = None

        line = f.readline()[:-2]
        line_counter[0] += 1
        if line[0] == '+':
            self.json = line[1:]
            if self.json == "QUEUED":
                self.queued = True
        elif line[0] == '-':
            self.json = line[1:]
            self.error = True
        elif line[0] == '$':
            self.json = str(f.read(int(line[1:])))
            f.read(2)  # read \r\n
            line_counter[0] += 1
        elif line[0] == ':':
            self.json = int(line[1:])
        elif line[0] == ',':
            self.json = float(line[1:])
        elif line[0] == '_':
            self.json = None
        elif line[0] == '#':
            self.json = line[1] == 't'
        elif line[0] == '!':
            self.json = str(f.read(int(line[1:])))
            f.read(2)  # read \r\n
            line_counter[0] += 1
            self.error = True
        elif line[0] == '=':
            self.json = str(f.read(int(line[1:])))[4:]   # skip "txt:" or "mkd:"
            f.read(2)  # read \r\n
            line_counter[0] += 1 + self.json.count("\r\n")
        elif line[0] == '(':
            self.json = line[1:]  # big-number is actually a string
        elif line[0] in ['*', '~', '>']:  # unfortunately JSON doesn't tell the difference between a list and a set
            self.json = []
            count = int(line[1:])
            for i in range(count):
                ele = Response(f, line_counter)
                self.json.append(ele.json)
        elif line[0] in ['%', '|']:
            self.json = {}
            count = int(line[1:])
            for i in range(count):
                field = Response(f, line_counter)
                # Redis allows fields to be non-strings but JSON doesn't.
                # Luckily, for any kind of response we can validate, the fields are
                # always strings (example: XINFO STREAM)
                # The reason we can't always convert to string is because of DEBUG PROTOCOL MAP
                # which anyway doesn't have a schema
                if isinstance(field.json, str):
                    field = field.json
                value = Response(f, line_counter)
                self.json[field] = value.json
            if line[0] == '|':
                # We don't care about the attributes, read the real response
                real_res = Response(f, line_counter)
                self.__dict__.update(real_res.__dict__)


    def __str__(self):
        return json.dumps(self.json)


def process_file(docs, path):
    """
    This function processes a single file generated by logreqres.c
    """
    line_counter = [0]  # A list with one integer: to force python to pass it by reference
    command_counter = dict()

    print(f"Processing {path} ...")

    # Convert file to StringIO in order to minimize IO operations
    with open(path, "r", newline="\r\n", encoding="latin-1") as f:
        content = f.read()

    with io.StringIO(content) as fakefile:
        while True:
            try:
                req = Request(fakefile, docs, line_counter)
                if not req.argv:
                    # EOF
                    break
                res = Response(fakefile, line_counter)
            except json.decoder.JSONDecodeError as err:
                print(f"JSON decoder error while processing {path}:{line_counter[0]}: {err}")
                print(traceback.format_exc())
                raise
            except Exception as err:
                print(f"General error while processing {path}:{line_counter[0]}: {err}")
                print(traceback.format_exc())
                raise

            if not req.command:
                # Unknown command
                continue

            command_counter[req.command] = command_counter.get(req.command, 0) + 1

            if res.error or res.queued:
                continue

            if req.command in IGNORED_COMMANDS:
                continue

            try:
                jsonschema.validate(instance=res.json, schema=req.schema, cls=schema_validator)
            except (jsonschema.ValidationError, jsonschema.exceptions.SchemaError) as err:
                print(f"JSON schema validation error on {path}: {err}")
                print(f"argv: {req.argv}")
                try:
                    print(f"Response: {res}")
                except UnicodeDecodeError as err:
                   print("Response: (unprintable)")
                print(f"Schema: {json.dumps(req.schema, indent=2)}")
                print(traceback.format_exc())
                raise

    return command_counter


def fetch_schemas(cli, port, args, docs):
    redis_proc = subprocess.Popen(args, stdout=subprocess.PIPE)

    while True:
        try:
            print('Connecting to RedQueue...')
            r = redis.Redis(port=port)
            r.ping()
            break
        except Exception as e:
            time.sleep(0.1)

    print('Connected')

    cli_proc = subprocess.Popen([cli, '-p', str(port), '--json', 'command', 'docs'], stdout=subprocess.PIPE)
    stdout, stderr = cli_proc.communicate()
    docs_response = json.loads(stdout)

    for name, doc in docs_response.items():
        if "subcommands" in doc:
            for subname, subdoc in doc["subcommands"].items():
                docs[subname] = subdoc
        else:
            docs[name] = doc

    redis_proc.terminate()
    redis_proc.wait()


if __name__ == '__main__':
    # Figure out where the sources are
    srcdir = os.path.abspath(os.path.dirname(os.path.abspath(__file__)) + "/../src")
    testdir = os.path.abspath(os.path.dirname(os.path.abspath(__file__)) + "/../tests")

    parser = argparse.ArgumentParser()
    parser.add_argument('--server', type=str, default='%s/redqueue-server' % srcdir)
    parser.add_argument('--port', type=int, default=6534)
    parser.add_argument('--cli', type=str, default='%s/redqueue-cli' % srcdir)
    parser.add_argument('--module', type=str, action='append', default=[])
    parser.add_argument('--verbose', action='store_true')
    parser.add_argument('--fail-commands-not-all-hit', action='store_true')
    parser.add_argument('--fail-missing-reply-schemas', action='store_true')
    args = parser.parse_args()

    docs = dict()

    # Fetch schemas from a Redis instance
    print('Starting RedQueue server')
    redis_args = [args.server, '--port', str(args.port)]
    for module in args.module:
        redis_args += ['--loadmodule', 'tests/modules/%s.so' % module]

    fetch_schemas(args.cli, args.port, redis_args, docs)

    # Fetch schemas from a sentinel
    print('Starting RedQueue sentinel')

    # Sentinel needs a config file to start
    config_file = "tmpsentinel.conf"
    open(config_file, 'a').close()

    sentinel_args = [args.server, config_file, '--port', str(args.port), "--sentinel"]
    fetch_schemas(args.cli, args.port, sentinel_args, docs)
    os.unlink(config_file)

    missing_schema = [k for k, v in docs.items()
                      if "reply_schema" not in v and k not in IGNORED_COMMANDS]
    if missing_schema:
        print("WARNING! The following commands are missing a reply_schema:")
        for k in sorted(missing_schema):
            print(f"  {k}")
        if args.fail_missing_reply_schemas:
            print("ERROR! at least one command does not have a reply_schema")
            sys.exit(1)

    start = time.time()

    # Obtain all the files to processes
    paths = []
    for path in glob.glob('%s/tmp/*/*.reqres' % testdir):
        paths.append(path)

    for path in glob.glob('%s/cluster/tmp/*/*.reqres' % testdir):
        paths.append(path)

    for path in glob.glob('%s/sentinel/tmp/*/*.reqres' % testdir):
        paths.append(path)

    counter = collections.Counter()
    # Spin several processes to handle the files in parallel
    with multiprocessing.Pool(multiprocessing.cpu_count()) as pool:
        func = partial(process_file, docs)
        # pool.map blocks until all the files have been processed
        for result in pool.map(func, paths):
            counter.update(result)
    command_counter = dict(counter)

    elapsed = time.time() - start
    print(f"Done. ({timedelta(seconds=elapsed)})")
    print("Hits per command:")
    for k, v in sorted(command_counter.items()):
        print(f"  {k}: {v}")
    not_hit = set(set(docs.keys()) - set(command_counter.keys()) - set(IGNORED_COMMANDS))
    if not_hit:
        if args.verbose:
            print("WARNING! The following commands were not hit at all:")
            for k in sorted(not_hit):
                print(f"  {k}")
        if args.fail_commands_not_all_hit:
            print("ERROR! at least one command was not hit by the tests")
            sys.exit(1)

