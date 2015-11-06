#!/usr/bin/python

# Copyright CEA/DAM 2015
# Author: Henri Doreau <henri.doreau@cea.fr>
#
# This file is part of the Phobos project

"""
Phobos command-line interface utilities.

Phobos action handlers (AH). AHs are descriptors of the objects that phobos can
manipulate. They expose both command line subparsers, to retrieve and validate
specific command line parameters and the API entry points for phobos to trigger
actions.
"""

import os
import sys
import errno
import argparse
import logging
import json
import yaml
import csv
import StringIO
import xml.etree.ElementTree
import xml.dom.minidom

import phobos.capi.clogging as pho_logging
import phobos.capi.dss as cdss

from phobos.config import load_config_file, get_config_value
from phobos.dss import Client
from ClusterShell.NodeSet import NodeSet


def csv_dump(data):
    outbuf = StringIO.StringIO()
    writer = csv.DictWriter(outbuf, data[0].keys())
    if sys.version_info >= (2, 7):
        writer.writeheader()
    else:
        writer.writerow(dict((i, i) for i in data[0].keys()))
    writer.writerows(data)
    out = outbuf.getvalue()
    outbuf.close()
    return out

def xml_dump(data, item_type='item'):
    top = xml.etree.ElementTree.Element('phobos')
    for item in data:
        children = xml.etree.ElementTree.Element(item_type, **item)
        top.append(children)
    rough_string = xml.etree.ElementTree.tostring(top)
    reparsed = xml.dom.minidom.parseString(rough_string)
    return reparsed.toprettyxml(indent="  ")

def human_dump(data, item_type='item'):
    title = " %s " % (item_type)
    out = " {0:_^50}\n".format(str(title))
    for item in data:
        vals = []
        for k in item:
            vals.append(" |{0:<20}|{1:<27}|".format(k,item[k]))
        out = out+ "\n".join(vals) + "\n {0:_^50}\n".format("")
    return out

def dump_object_list(objs, fmt="human", numeric=False):
    """
    Helper for user friendly object display.
    """
    display = {
    cdss.dev_info:('serial', ['adm_status', 'changer_idx', 'family',
                   'host', 'model', 'path', 'serial']),
    cdss.media_info:('model',['adm_status', 'fs_status', 'fs_type'])
    }

    formats = {
    'json' : json.dumps,
    'yaml' : yaml.dump,
    'xml'  : xml_dump,
    'csv'  : csv_dump,
    'human': human_dump,
    }

    formatter = formats.get(fmt)
    if formatter is None:
        logger = logging.getLogger(__name__)
        logger.error("Unkown output format:", format)
        return

    #Is an instance of a known class ?
    dclass = None
    for k in display:
        if isinstance(objs[0], k):
            dclass = k

    if not dclass:
        logger = logging.getLogger(__name__)
        logger.error("No model found to display this class:", objs[0].__class__.__name__)
        return

    objlist=[]
    #Build a dict with attributs to export/output
    for obj in objs:
        objext={}
        for k in display[dclass][1]:
            if not numeric and hasattr(cdss, '%s2str' % k):
                method = getattr(cdss, '%s2str' % k )
                objext[k] = method(getattr(obj,k))
            else:
                objext[k] = str(getattr(obj,k))
        objlist.append(objext)

    #Print formatted objects
    print formatter(objlist)

def phobos_log_handler(rec):
    """
    Receive log records emitted from lower layers and inject them into the
    currently configured logger.
    """
    full_msg = rec[6]

    # Append ': <errmsg>' to the original message if err_code was set
    if rec[4] != 0:
        full_msg += ": %s"
        args = (os.strerror(rec[4]), )
    else:
        args = tuple()

    attrs = {
        'name': 'internals',
        'levelno': rec[0],
        'levelname': logging.getLevelName(rec[0]),
        'filename': rec[1],
        'funcName': rec[2],
        'lineno': rec[3],
        'exc_info': None,
        'msg': full_msg,
        'args': args,
        'created': rec[5],
    }

    record = logging.makeLogRecord(attrs)
    logger = logging.getLogger(__name__)
    logger.handle(record)

class BaseOptHandler(object):
    """
    Skeleton for action handlers. It can register a corresponding argument
    subparser to a top-level one, with targeted object, description and
    supported actions.
    """
    label = '(undef)'
    descr = '(undef)'
    verbs = []

    def __init__(self, params, **kwargs):
        """
        Initialize action handler with command line parameters. These are to be
        re-checked later by the specialized chk_* methods.
        """
        super(BaseOptHandler, self).__init__(**kwargs)
        self.params = params
        self.client = None
        self.logger = logging.getLogger(__name__)

    def dss_connect(self):
        """Initialize a DSS Client."""
        # XXX Connection info is currently expressed as an opaque string.
        #     Thus use the special _connect keyword to not rebuild it.
        self.client = Client()
        conn_info = get_config_value('dss', 'connect_string')
        if conn_info is None:
            conn_info = ''
        self.client.connect(_connect=conn_info)

    def dss_disconnect(self):
        """Release resources associated to a DSS handle."""
        self.client.disconnect()

    @classmethod
    def add_options(cls, parser):
        """Add options for this specific command-line subsection."""
        pass

    @classmethod
    def subparser_register(cls, base_parser):
        """Register the subparser to a top-level one."""
        subparser = base_parser.add_parser(cls.label, help=cls.descr)

        # Register options relating to the current media
        cls.add_options(subparser)

        # Register supported verbs and associated options
        if cls.verbs:
            v_parser = subparser.add_subparsers(dest='verb')
            for verb in cls.verbs:
                verb.subparser_register(v_parser)

        return subparser


class AddOptHandler(BaseOptHandler):
    """Insert a new resource into the system."""
    label = 'add'
    descr = 'insert new resource(s) to the system'

    @classmethod
    def add_options(cls, parser):
        super(AddOptHandler, cls).add_options(parser)
        parser.add_argument('--unlock', action='store_true',
                            help='Unlock resource on success')
        parser.add_argument('res', nargs='+', help='Resource(s) to add')


class CheckOptHandler(BaseOptHandler):
    """Issue a check command on the designated object(s)."""
    label = 'check'
    descr = 'check state / consistency of the selected resource(s).'

    @classmethod
    def add_options(cls, parser):
        """Add resource-specific options."""
        super(CheckOptHandler, cls).add_options(parser)
        parser.add_argument('res', nargs='+', help='Resource(s) to check')


class ListOptHandler(BaseOptHandler):
    """List items of a specific type."""
    label = 'list'
    descr = 'list all entries of the kind'


class LockOptHandler(BaseOptHandler):
    """Lock resource."""
    label = 'lock'
    descr = 'lock resource(s)'

    @classmethod
    def add_options(cls, parser):
        """Add resource-specific options."""
        super(LockOptHandler, cls).add_options(parser)
        parser.add_argument('res', nargs='+', help='Resource(s) to lock')
        parser.add_argument('--force', action='store_true',
                            help='Do not check the current lock state')

class UnlockOptHandler(BaseOptHandler):
    """Unlock resource."""
    label = 'unlock'
    descr = 'unlock resource(s)'

    @classmethod
    def add_options(cls, parser):
        """Add resource-specific options."""
        super(UnlockOptHandler, cls).add_options(parser)
        parser.add_argument('res', nargs='+', help='Resource(s) to unlock')
        parser.add_argument('--force', action='store_true',
                            help='Do not check the current lock state')

class ShowOptHandler(BaseOptHandler):
    """Show resource details."""
    label = 'show'
    descr = 'show resource details'

    @classmethod
    def add_options(cls, parser):
        """Add resource-specific options."""
        super(ShowOptHandler, cls).add_options(parser)
        parser.add_argument('res', nargs='+', help='Resource(s) to show')
        parser.add_argument('--numeric', action='store_true',
                            help='Output numeric values')
        parser.add_argument('--format', default='human',
                            help="Output format human/xml/json/csv/yaml " \
                                 "(default: human)")

class DriveListOptHandler(ListOptHandler):
    """
    Specific version of the 'list' command for tape drives, with a couple
    extra-options.
    """
    @classmethod
    def add_options(cls, parser):
        """Add resource-specific options."""
        super(DriveListOptHandler, cls).add_options(parser)
        parser.add_argument('-t', '--type', help='filter on type')


class TapeAddOptHandler(AddOptHandler):
    """Specific version of the 'add' command for tapes, with extra-options."""
    @classmethod
    def add_options(cls, parser):
        """Add resource-specific options."""
        super(TapeAddOptHandler, cls).add_options(parser)
        parser.add_argument('-t', '--type', required=True, help='tape technology')
        parser.add_argument('-f', '--fs-type', help='Filesystem type')


class FormatOptHandler(BaseOptHandler):
    """Format a resource."""
    label = 'format'
    descr = 'format a tape'

    @classmethod
    def add_options(cls, parser):
        """Add resource-specific options."""
        super(FormatOptHandler, cls).add_options(parser)
        parser.add_argument('-f', '--fs-type', required=True, help='Filesystem type')
        parser.add_argument('-n', '--nb-streams', metavar='STREAMS', type=int,
                            help='Max number of parallel formatting operations')
        parser.add_argument('--unlock', action='store_true',
                            help='Unlock tape once it is ready to be written')
        parser.add_argument('res', nargs='+', help='Resource(s) to format')


class DirOptHandler(BaseOptHandler):
    """Directory-related options and actions."""
    label = 'dir'
    descr = 'handle directories'
    verbs = [
        AddOptHandler,
        ListOptHandler,
        ShowOptHandler,
        LockOptHandler,
        UnlockOptHandler
    ]

    def exec_add(self):
        print 'DIR ADD'

    def exec_list(self):
        """List directories as devices."""
        for obj in self.client.devices.get(family='dir'):
            print obj.serial

    def exec_show(self):
        """Show directory details."""
        dirs=[]
        for serial in self.params.get('res'):
            wdir = self.client.devices.get(family='dir', serial=serial)
            if not wdir:
                self.logger.warning("Serial %s not found" % serial)
                continue
            assert len(wdir) == 1
            dirs.append(wdir[0])
        dump_object_list(dirs, self.params.get('format'), \
                         self.params.get('numeric'))


    def exec_lock(self):
        print 'DIR LOCK'

    def exec_unlock(self):
        print 'DIR UNLOCK'


class DriveOptHandler(BaseOptHandler):
    """Tape Drive options and actions."""
    label = 'drive'
    descr = 'handle tape drives (use ID or device path to identify resource)'
    verbs = [
        AddOptHandler,
        DriveListOptHandler,
        ShowOptHandler,
        LockOptHandler,
        UnlockOptHandler
    ]

    def exec_add(self):
        print 'DRIVE ADD'

    def exec_list(self):
        """List all drives."""
        # Clarification: Tape is a kind of drive
        for drive in self.client.devices.get(family='tape'):
            print drive.serial

    def exec_show(self):
        """Show drive details."""
        drives = []
        for serial in self.params.get('res'):
            drive = self.client.devices.get(family='tape', serial=serial)
            if not drive:
                self.logger.warning("Serial %s not found" % serial)
                continue
            assert len(drive) == 1
            drives.append(drive[0])
        dump_object_list(drives, self.params.get('format'),\
                         self.params.get('numeric'))

    def exec_lock(self):
        """Drive lock"""
        drives = []
        serials = self.params.get('res')
        for serial in serials:
            drive = self.client.devices.get(serial=serial)
            if drive[0].lock.lock != "":
                if self.params.get('force'):
                    self.logger.warn("Drive %s is in use. Administrative" \
                            " locking will not be effective immediately" % serial)
                else:
                    self.logger.error("Drive %s is in use by %s." % \
                                       (serial, drive[0].lock.lock));
                    continue
            drive[0].adm_status = cdss.PHO_DEV_ADM_ST_LOCKED
            drives.append(drive[0])
        if len(drives) == len(serials):
            rc = self.client.devices.update(drives)
        else:
            rc = errno.EPERM
            self.logger.error("One or more drives are in use, use --force")

        if not rc:
            print "%d drive(s) locked" % len(drives)
        else:
            self.logger.error("Failed to lock one or more drive(s), error: %s" %
                             os.strerror(abs(rc)))

    def exec_unlock(self):
        """Drive unlock"""
        drives = []
        serials = self.params.get('res')
        for serial in serials:
            drive = self.client.devices.get(serial=serial)
            if drive[0].lock.lock != "" and not self.params.get('force'):
                self.logger.error("Drive %s is in use by %s." % \
                                  (serial, drive[0].lock.lock));
                continue
            if drive[0].adm_status == cdss.PHO_DEV_ADM_ST_UNLOCKED:
                self.logger.warn("Drive %s is already unlocked" % serial)
            drive[0].adm_status = cdss.PHO_DEV_ADM_ST_UNLOCKED
            drives.append(drive[0])
        if len(drives) == len(serials):
            rc = self.client.devices.update(drives)
        else:
            rc = errno.EPERM
            self.logger.error("One or more drives are in use, use --force")

        if not rc:
            print "%d drive(s) unlocked" % len(drives)
        else:
            self.logger.error("Failed to unlock one or more drive(s), error: %s" %
                             os.strerror(abs(rc)))

class TapeOptHandler(BaseOptHandler):
    """Magnetic tape options and actions."""
    label = 'tape'
    descr = 'handle magnetic tape (use tape label to identify resource)'
    verbs = [
        TapeAddOptHandler,
        FormatOptHandler,
        ShowOptHandler,
        ListOptHandler,
        LockOptHandler,
        UnlockOptHandler
    ]

    def exec_add(self):
        """Add new tapes"""
        media_list = []
        tapes = NodeSet.fromlist(self.params.get('res'))

        for tape in tapes:
            media = cdss.media_info()
            media.fs_type = cdss.str2fs_type(self.params.get('fs_type'))
            media.model = self.params.get('type')
            media.addr_type = cdss.PHO_ADDR_PATH
            media.id.type = cdss.PHO_DEV_TAPE
            if self.params.get('unlock'):
                media.adm_status = cdss.PHO_DEV_ADM_ST_UNLOCKED
            else:
                media.adm_status = cdss.PHO_DEV_ADM_ST_LOCKED
            media.stats =  cdss.media_stats()
            cdss.media_id_set(media.id, tape)
            media_list.append(media)

        rc = self.client.media.insert(media_list)
        if not rc:
            print "%d tape(s) added" % len(media_list)
        else:
            self.logger.error("Failed to add tape(s), error: %s" % os.strerror(abs(rc)))

    def exec_format(self):
        print 'TAPE FORMAT'

    def exec_show(self):
        """Show tape details."""
        tapes = []
        uids = NodeSet.fromlist(self.params.get('res'))
        for uid in uids:
            tape = self.client.media.get(family='tape', id=uid)
            if not tape:
                self.logger.warning("Tape id %s not found" % uid)
                continue
            assert len(tape) == 1
            tapes.append(tape[0])
        dump_object_list(tapes, self.params.get('format'), \
                         self.params.get('numeric'))

    def exec_list(self):
        """List all tapes."""
        for tape in self.client.media.get(family='tape'):
            print cdss.media_id_get(tape.id)

    def exec_lock(self):
        print 'Tape lock'
        tapes = []
        uids = NodeSet.fromlist(self.params.get('res'))
        for uid in uids:
            tape = self.client.media.get(id=uid)

            if tape[0].lock.lock != "":
                if self.params.get('force'):
                    self.logger.warn("Tape %s is in use. Administrative" \
                            " locking will not be effective immediately" % uid)
                else:
                    self.logger.error("Tape %s is in use by %s." % \
                                     (uid, tape[0].lock.lock));
                    continue

            tape[0].adm_status = cdss.PHO_MDA_ADM_ST_LOCKED
            tapes.append(tape[0])

        if len(tapes) == len(uids):
            rc = self.client.media.update(tapes)
        else:
            rc = errno.EPERM
            self.logger.error("One or more tapes are in use, use --force")

        if not rc:
            print "%d tape(s) locked" % len(tapes)
        else:
            self.logger.error("Failed to lock one or more tape(s), error: %s" %
                             os.strerror(abs(rc)))

    def exec_unlock(self):
        print 'Tape unlock'
        tapes = []
        uids = NodeSet.fromlist(self.params.get('res'))
        for uid in uids:
            tape = self.client.media.get(id=uid)

            if tape[0].lock.lock != "" and not self.params.get('force'):
                self.logger.error("Tape %s is in use by %s." % \
                                  (uid, tape[0].lock.lock));
                continue

            if tape[0].adm_status == cdss.PHO_MDA_ADM_ST_UNLOCKED:
                self.logger.warn("Tape %s is already unlocked" % uid)

            tape[0].adm_status = cdss.PHO_MDA_ADM_ST_UNLOCKED
            tapes.append(tape[0])

        if len(tapes) == len(uids):
            rc = self.client.media.update(tapes)
        else:
            rc = errno.EPERM
            self.logger.error("One or more tapes are not locked, use --force")

        if not rc:
            print "%d tape(s) unlocked" % len(tapes)
        else:
            self.logger.error("Failed to unlock one or more tape(s), error: %s" %
                             os.strerror(abs(rc)))

class PhobosActionContext(object):
    """
    Find, initialize and operate an appropriate action execution context for the
    specified command line.
    """
    CLI_LOG_FORMAT_REG = "%(asctime)s <%(levelname)s> %(message)s"
    CLI_LOG_FORMAT_DEV = "%(asctime)s <%(levelname)s> " \
                         "[%(funcName)s:%(filename)s:%(lineno)d] %(message)s"

    default_conf_file = '/etc/phobos.conf'
    supported_objects = [ DirOptHandler, DriveOptHandler, TapeOptHandler ]

    def __init__(self, args, **kwargs):
        """Initialize a PAC instance."""
        super(PhobosActionContext, self).__init__(**kwargs)
        self.parser = None
        self.parameters = None

        self.install_arg_parser()
        self.parameters = vars(self.parser.parse_args(args))

        self.load_config() # After this, code can use get_config_value()

    def install_arg_parser(self):
        """Initialize hierarchical command line parser."""
        # Top-level parser for common options
        self.parser = argparse.ArgumentParser('phobos',
                                              description= \
                                              'phobos command line interface')

        verb_grp = self.parser.add_mutually_exclusive_group()
        verb_grp.add_argument('-v', '--verbose', help='Increase verbosity',
                                 action='count', default=0)
        verb_grp.add_argument('-q', '--quiet', help='Decrease verbosity',
                                 action='count', default=0)

        self.parser.add_argument('-c', '--config',
                                 default=self.default_conf_file,
                                 help='Alternative configuration file')

        # Initialize specialized (objects) argument parsers
        sub = self.parser.add_subparsers(dest='media',
                                         help="Select target object type")

        for obj in self.supported_objects:
            obj.subparser_register(sub)

    def load_config(self):
        """Load configuration file."""
        cpath = self.parameters.get('config')
        # Try to open configuration file
        try:
            load_config_file(cpath)
        except IOError as exc:
            if exc.errno == errno.ENOENT or exc.errno == errno.EALREADY:
                return
            raise

    def configure_app_logging(self):
        """
        Configure a multilayer logger according to command line specifications.
        """
        fmt = self.CLI_LOG_FORMAT_REG # default

        # Both are mutually exclusive
        lvl = self.parameters.get('verbose')
        lvl -= self.parameters.get('quiet')

        if lvl >= 2:
            # -vv
            pylvl = logging.DEBUG
            fmt = self.CLI_LOG_FORMAT_DEV
        elif lvl == 1:
            # -v
            pylvl = logging.INFO
        elif lvl == 0:
            # default
            pylvl = logging.WARNING
        elif lvl == -1:
            # -q
            pylvl = logging.ERROR
        elif lvl <= -2:
            # -qq
            pylvl = logging.NOTSET

        pho_logging.set_callback(phobos_log_handler)
        pho_logging.set_level(pylvl)

        logger = logging.getLogger(__name__)
        logging.basicConfig(level=pylvl, format=fmt)

    def run(self):
        """
        Invoke the desired method on the selected media handler.
        It is assumed that all checks have happened already to make sure that
        the execution order refers to a valid method of the target object.
        """
        self.configure_app_logging()

        target = self.parameters.get('media')
        action = self.parameters.get('verb')

        assert target is not None
        assert action is not None

        target_inst = None
        for obj in self.supported_objects:
            if obj.label == target:
                target_inst = obj(self.parameters)
                break

        # The command line parser must catch such mistakes
        assert target_inst is not None

        # Invoke target::exec_{action}()
        target_inst.dss_connect()
        getattr(target_inst, 'exec_%s' % action)()
        target_inst.dss_disconnect()

def phobos_main(args=sys.argv[1::]):
    """
    Entry point for the `phobos' command. Indirectly provides
    argument parsing and execution of the appropriate actions.
    """
    PhobosActionContext(args).run()
