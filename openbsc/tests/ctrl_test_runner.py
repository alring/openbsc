#!/usr/bin/env python

# (C) 2013 by Jacob Erlbeck <jerlbeck@sysmocom.de>
# based on vty_test_runner.py:
# (C) 2013 by Katerina Barone-Adesi <kat.obsc@gmail.com>
# (C) 2013 by Holger Hans Peter Freyther
# based on bsc_control.py.

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import os
import time
import unittest
import socket
import sys
import struct

import osmopy.obscvty as obscvty
import osmopy.osmoutil as osmoutil

confpath = '.'
verbose = False

class TestCtrlBase(unittest.TestCase):

    def ctrl_command(self):
        raise Exception("Needs to be implemented by a subclass")

    def ctrl_app(self):
        raise Exception("Needs to be implemented by a subclass")

    def setUp(self):
        osmo_ctrl_cmd = self.ctrl_command()[:]
        config_index = osmo_ctrl_cmd.index('-c')
        if config_index:
            cfi = config_index + 1
            osmo_ctrl_cmd[cfi] = os.path.join(confpath, osmo_ctrl_cmd[cfi])

        try:
            print "Launch: %s from %s" % (' '.join(osmo_ctrl_cmd), os.getcwd())
            self.proc = osmoutil.popen_devnull(osmo_ctrl_cmd)
        except OSError:
            print >> sys.stderr, "Current directory: %s" % os.getcwd()
            print >> sys.stderr, "Consider setting -b"
        time.sleep(2)

        appstring = self.ctrl_app()[2]
        appport = self.ctrl_app()[0]
        self.connect("127.0.0.1", appport)
        self.next_id = 1000

    def tearDown(self):
        self.disconnect()
        osmoutil.end_proc(self.proc)

    def prefix_ipa_ctrl_header(self, data):
        return struct.pack(">HBB", len(data)+1, 0xee, 0) + data

    def remove_ipa_ctrl_header(self, data):
        if (len(data) < 4):
            raise BaseException("Answer too short!")
        (plen, ipa_proto, osmo_proto) = struct.unpack(">HBB", data[:4])
        if (plen + 3 > len(data)):
            print "Warning: Wrong payload length (expected %i, got %i)" % (plen, len(data) - 3)
        if (ipa_proto != 0xee or osmo_proto != 0):
            raise BaseException("Wrong protocol in answer!")

        return data[4:plen+3], data[plen+3:]

    def disconnect(self):
        if not (self.sock is None):
            self.sock.close()

    def connect(self, host, port):
        if verbose:
            print "Connecting to host %s:%i" % (host, port)

        sck = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sck.setblocking(1)
        sck.connect((host, port))
        self.sock = sck
        return sck

    def send(self, data):
        if verbose:
            print "Sending \"%s\"" %(data)
        data = self.prefix_ipa_ctrl_header(data)
        return self.sock.send(data) == len(data)

    def send_set(self, var, value, id):
        setmsg = "SET %s %s %s" %(id, var, value)
        return self.send(setmsg)

    def send_get(self, var, id):
        getmsg = "GET %s %s" %(id, var)
        return self.send(getmsg)

    def do_set(self, var, value):
        id = self.next_id
        self.next_id += 1
        self.send_set(var, value, id)
        return self.recv_msgs()[id]

    def do_get(self, var):
        id = self.next_id
        self.next_id += 1
        self.send_get(var, id)
        return self.recv_msgs()[id]

    def recv_msgs(self):
        responses = {}
        data = self.sock.recv(4096)
        while (len(data)>0):
            (answer, data) = self.remove_ipa_ctrl_header(data)
            if verbose:
                print "Got message:", answer
            (mtype, id, msg) = answer.split(None, 2)
            id = int(id)
            rsp = {'mtype': mtype, 'id': id}
            if mtype == "ERROR":
                rsp['error'] = msg
            else:
                [rsp['var'], rsp['value']]  = msg.split(None, 2)

            responses[id] = rsp

        if verbose:
            print "Decoded replies: ", responses

        return responses


class TestCtrlBSC(TestCtrlBase):

    def tearDown(self):
        TestCtrlBase.tearDown(self)
        os.unlink("tmp_dummy_sock")

    def ctrl_command(self):
        return ["./src/osmo-bsc/osmo-bsc", "-r", "tmp_dummy_sock", "-c",
                "doc/examples/osmo-bsc/osmo-bsc.cfg"]

    def ctrl_app(self):
        return (4249, "./src/osmo-bsc/osmo-bsc", "OsmoBSC", "bsc")

    def testCtrlErrs(self):
        r = self.do_get('invalid')
        self.assertEquals(r['mtype'], 'ERROR')
        self.assertEquals(r['error'], 'Command not found')

        r = self.do_set('rf_locked', '999')
        self.assertEquals(r['mtype'], 'ERROR')
        self.assertEquals(r['error'], 'Value failed verification.')

        r = self.do_get('bts')
        self.assertEquals(r['mtype'], 'ERROR')
        self.assertEquals(r['error'], 'Error while parsing the index.')

        r = self.do_get('bts.999')
        self.assertEquals(r['mtype'], 'ERROR')
        self.assertEquals(r['error'], 'Error while resolving object')

    def testRfLock(self):
        r = self.do_get('bts.0.rf_state')
        self.assertEquals(r['mtype'], 'GET_REPLY')
        self.assertEquals(r['var'], 'bts.0.rf_state')
        self.assertEquals(r['value'], 'inoperational,unlocked,on')

        r = self.do_set('rf_locked', '1')
        self.assertEquals(r['mtype'], 'SET_REPLY')
        self.assertEquals(r['var'], 'rf_locked')
        self.assertEquals(r['value'], '1')

        time.sleep(1.5)

        r = self.do_get('bts.0.rf_state')
        self.assertEquals(r['mtype'], 'GET_REPLY')
        self.assertEquals(r['var'], 'bts.0.rf_state')
        self.assertEquals(r['value'], 'inoperational,locked,off')

        r = self.do_set('rf_locked', '0')
        self.assertEquals(r['mtype'], 'SET_REPLY')
        self.assertEquals(r['var'], 'rf_locked')
        self.assertEquals(r['value'], '0')

        time.sleep(1.5)

        r = self.do_get('bts.0.rf_state')
        self.assertEquals(r['mtype'], 'GET_REPLY')
        self.assertEquals(r['var'], 'bts.0.rf_state')
        self.assertEquals(r['value'], 'inoperational,unlocked,on')

    def testTimezone(self):
        r = self.do_get('bts.0.timezone')
        self.assertEquals(r['mtype'], 'GET_REPLY')
        self.assertEquals(r['var'], 'bts.0.timezone')
        self.assertEquals(r['value'], 'off')

        r = self.do_set('bts.0.timezone', '-2,15,2')
        self.assertEquals(r['mtype'], 'SET_REPLY')
        self.assertEquals(r['var'], 'bts.0.timezone')
        self.assertEquals(r['value'], '-2,15,2')

        r = self.do_get('bts.0.timezone')
        self.assertEquals(r['mtype'], 'GET_REPLY')
        self.assertEquals(r['var'], 'bts.0.timezone')
        self.assertEquals(r['value'], '-2,15,2')

        # Test invalid input
        r = self.do_set('bts.0.timezone', '-2,15,2,5,6,7')
        self.assertEquals(r['mtype'], 'SET_REPLY')
        self.assertEquals(r['var'], 'bts.0.timezone')
        self.assertEquals(r['value'], '-2,15,2')

        r = self.do_set('bts.0.timezone', '-2,15')
        self.assertEquals(r['mtype'], 'ERROR')
        r = self.do_set('bts.0.timezone', '-2')
        self.assertEquals(r['mtype'], 'ERROR')
        r = self.do_set('bts.0.timezone', '1')

        r = self.do_set('bts.0.timezone', 'off')
        self.assertEquals(r['mtype'], 'SET_REPLY')
        self.assertEquals(r['var'], 'bts.0.timezone')
        self.assertEquals(r['value'], 'off')

        r = self.do_get('bts.0.timezone')
        self.assertEquals(r['mtype'], 'GET_REPLY')
        self.assertEquals(r['var'], 'bts.0.timezone')
        self.assertEquals(r['value'], 'off')

def add_bsc_test(suite, workdir):
    if not os.path.isfile(os.path.join(workdir, "src/osmo-bsc/osmo-bsc")):
        print("Skipping the BSC test")
        return
    test = unittest.TestLoader().loadTestsFromTestCase(TestCtrlBSC)
    suite.addTest(test)

if __name__ == '__main__':
    import argparse
    import sys

    workdir = '.'

    parser = argparse.ArgumentParser()
    parser.add_argument("-v", "--verbose", dest="verbose",
                        action="store_true", help="verbose mode")
    parser.add_argument("-p", "--pythonconfpath", dest="p",
                        help="searchpath for config")
    parser.add_argument("-w", "--workdir", dest="w",
                        help="Working directory")
    args = parser.parse_args()

    verbose_level = 1
    if args.verbose:
        verbose_level = 2
        verbose = True

    if args.w:
        workdir = args.w

    if args.p:
        confpath = args.p

    print "confpath %s, workdir %s" % (confpath, workdir)
    os.chdir(workdir)
    print "Running tests for specific control commands"
    suite = unittest.TestSuite()
    add_bsc_test(suite, workdir)
    res = unittest.TextTestRunner(verbosity=verbose_level).run(suite)
    sys.exit(len(res.errors) + len(res.failures))
