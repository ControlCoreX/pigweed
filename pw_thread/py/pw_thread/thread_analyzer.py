# Copyright 2021 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Library to analyze and dump Thread protos and Thread snapshots into text."""

from typing import Optional, List, Mapping
import pw_tokenizer
from pw_tokenizer import proto as proto_detokenizer
from pw_thread_protos import thread_pb2

THREAD_STATE_TO_STRING: Mapping[int, str] = {
    thread_pb2.ThreadState.Enum.UNKNOWN: 'UNKNOWN',
    thread_pb2.ThreadState.Enum.INTERRUPT_HANDLER: 'INTERRUPT_HANDLER',
    thread_pb2.ThreadState.Enum.RUNNING: 'RUNNING',
    thread_pb2.ThreadState.Enum.READY: 'READY',
    thread_pb2.ThreadState.Enum.SUSPENDED: 'SUSPENDED',
    thread_pb2.ThreadState.Enum.BLOCKED: 'BLOCKED',
    thread_pb2.ThreadState.Enum.INACTIVE: 'INACTIVE',
}


def process_snapshot(serialized_snapshot: bytes,
                     tokenizer_db: Optional[pw_tokenizer.Detokenizer]) -> str:
    """Processes snapshot threads, producing a multi-line string."""
    captured_threads = thread_pb2.SnapshotThreadInfo()
    captured_threads.ParseFromString(serialized_snapshot)

    return str(ThreadSnapshotAnalyzer(captured_threads, tokenizer_db))


class StackInfo:
    """Provides stack-related information that can be inferred from a Thread."""

    _UNKNOWN_VALUE_STR = '0x' + '?' * 8

    def __init__(self, thread: thread_pb2.Thread):
        self._thread = thread

    def _stack_size_limit_limit_str(self) -> str:
        if not self.has_stack_size_limit():
            return 'size unknown'

        return f'{self.stack_size_limit()} bytes'

    def _stack_used_str(self) -> str:
        if not self.has_stack_used():
            return 'size unknown'

        used_str = f'{self.stack_used()} bytes'
        if not self.has_stack_size_limit():
            return used_str
        used_str += f', {100*self.stack_used()/self.stack_size_limit():.2f}%'
        return used_str

    def _stack_used_range_str(self) -> str:
        start_str = (f'0x{self._thread.stack_start_pointer:08x}'
                     if self._thread.HasField('stack_start_pointer') else
                     StackInfo._UNKNOWN_VALUE_STR)
        end_str = (f'0x{self._thread.stack_pointer:08x}'
                   if self._thread.HasField('stack_pointer') else
                   StackInfo._UNKNOWN_VALUE_STR)

        # TODO(amontanez): Would be nice to represent stack growth direction.
        return f'{start_str} - {end_str} ({self._stack_used_str()})'

    def _stack_limit_range_str(self) -> str:
        start_str = (f'0x{self._thread.stack_start_pointer:08x}'
                     if self._thread.HasField('stack_start_pointer') else
                     StackInfo._UNKNOWN_VALUE_STR)
        end_str = (f'0x{self._thread.stack_end_pointer:08x}'
                   if self._thread.HasField('stack_end_pointer') else
                   StackInfo._UNKNOWN_VALUE_STR)

        # TODO(amontanez): Would be nice to represent stack growth direction.
        return f'{start_str} - {end_str} ({self._stack_size_limit_limit_str()})'

    def _stack_pointer_str(self) -> str:
        return (f'0x{self._thread.stack_end_pointer:08x}'
                if self._thread.HasField('stack_pointer') else
                StackInfo._UNKNOWN_VALUE_STR)

    def has_stack_size_limit(self) -> bool:
        """Returns true if there's enough info to calculate stack size."""
        return (self._thread.HasField('stack_start_pointer')
                and self._thread.HasField('stack_end_pointer'))

    def stack_size_limit(self) -> int:
        """Returns the stack size limit in bytes.

        Precondition:
            has_stack_size_limit() must be true.
        """
        assert self.has_stack_size_limit(), 'Missing stack size information'
        return abs(self._thread.stack_start_pointer -
                   self._thread.stack_end_pointer)

    def has_stack_used(self) -> bool:
        """Returns true if there's enough info to calculate stack usage."""
        return (self._thread.HasField('stack_start_pointer')
                and self._thread.HasField('stack_pointer'))

    def stack_used(self) -> int:
        """Returns the stack usage in bytes.

        Precondition:
            has_stack_used() must be true.
        """
        assert self.has_stack_used(), 'Missing stack usage information'
        return abs(self._thread.stack_start_pointer -
                   self._thread.stack_pointer)

    def __str__(self) -> str:
        output = [
            'Stack info',
            f'  Stack used:   {self._stack_used_range_str()}',
            f'  Stack limits: {self._stack_limit_range_str()}',
        ]
        return '\n'.join(output)


class ThreadSnapshotAnalyzer:
    """This class simplifies dumping contents of a snapshot Metadata message."""
    def __init__(self,
                 threads: thread_pb2.SnapshotThreadInfo,
                 tokenizer_db: Optional[pw_tokenizer.Detokenizer] = None):
        self._threads = threads.threads
        self._tokenizer_db = (tokenizer_db if tokenizer_db is not None else
                              pw_tokenizer.Detokenizer(None))
        for thread in self._threads:
            proto_detokenizer.detokenize_fields(self._tokenizer_db, thread)

    def active_thread(self) -> Optional[thread_pb2.Thread]:
        """The thread that requested the snapshot capture."""
        running_thread = None
        for thread in self._threads:
            if thread.active:
                return thread
            if thread.state == thread_pb2.ThreadState.Enum.RUNNING:
                running_thread = thread

        return running_thread

    def __str__(self) -> str:
        """outputs a pw.snapshot.Metadata proto as a multi-line string."""
        output: List[str] = []
        if not self._threads:
            return ''

        output.append('Thread State')
        plural = '' if len(self._threads) == 1 else 's'
        thread_state_overview = f'  {len(self._threads)} thread{plural} running'
        requesting_thread = self.active_thread()
        if not requesting_thread:
            thread_state_overview += '.'
            output.append(thread_state_overview)
        else:
            thread_state_overview += ', '
            underline = (' ' * len(thread_state_overview) +
                         '~' * len(requesting_thread.name.decode()))
            thread_state_overview += (f'{requesting_thread.name.decode()}'
                                      ' active at the time of capture.')
            output.append(thread_state_overview)
            output.append(underline)

        output.append('')

        # Put the active thread at the front.
        requesting_thread = self.active_thread()
        if requesting_thread is not None:
            self._threads.remove(requesting_thread)
            self._threads.insert(0, requesting_thread)

        for thread in self._threads:
            thread_name = thread.name.decode()
            if not thread_name:
                thread_name = '[unnamed thread]'
            thread_headline = ('Thread '
                               f'({THREAD_STATE_TO_STRING[thread.state]}): '
                               f'{thread_name}')
            if self.active_thread() == thread:
                thread_headline += ' <-- [ACTIVE]'
            output.append(thread_headline)
            output.append(str(StackInfo(thread)))
            # Blank line between threads for nicer formatting.
            output.append('')

        return '\n'.join(output)
