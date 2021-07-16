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
"""Defines a callback-based RPC ClientImpl to use with pw_rpc.Client.

callback_client.Impl supports invoking RPCs synchronously or asynchronously.
Asynchronous invocations use a callback.

Synchronous invocations look like a function call:

.. code-block:: python

  status, response = client.channel(1).call.MyServer.MyUnary(some_field=123)

  # Streaming calls return an iterable of responses
  for reply in client.channel(1).call.MyService.MyServerStreaming(request):
      pass

Asynchronous invocations pass a callback in addition to the request. The
callback must be a callable that accepts a status and a payload, either of
which may be None. The Status is only set when the RPC is completed.

.. code-block:: python

  callback = lambda status, payload: print('Response:', status, payload)

  call = client.channel(1).call.MyServer.MyUnary.invoke(
      callback, some_field=123)

  call = client.channel(1).call.MyService.MyServerStreaming.invoke(
      callback, request):

When invoking a method, requests may be provided as a message object or as
kwargs for the message fields (but not both).
"""

import enum
import inspect
import logging
import queue
import textwrap
import threading
from typing import Any, Callable, Iterator, List, NamedTuple, Union, Optional

from pw_protobuf_compiler.python_protos import proto_repr
from pw_status import Status
from google.protobuf.message import Message

from pw_rpc import client, descriptors
from pw_rpc.client import PendingRpc, PendingRpcs
from pw_rpc.descriptors import Channel, Method, Service

_LOG = logging.getLogger(__name__)


class UseDefault(enum.Enum):
    """Marker for args that should use a default value, when None is valid."""
    VALUE = 0


OptionalTimeout = Union[UseDefault, float, None]

ResponseCallback = Callable[[PendingRpc, Any], Any]
CompletionCallback = Callable[[PendingRpc, Status], Any]
ErrorCallback = Callable[[PendingRpc, Status], Any]


class _Callbacks(NamedTuple):
    response: ResponseCallback
    completion: CompletionCallback
    error: ErrorCallback


def _default_response(rpc: PendingRpc, response: Any) -> None:
    _LOG.info('%s response: %s', rpc, response)


def _default_completion(rpc: PendingRpc, status: Status) -> None:
    _LOG.info('%s finished: %s', rpc, status)


def _default_error(rpc: PendingRpc, status: Status) -> None:
    _LOG.error('%s error: %s', rpc, status)


class _MethodClient:
    """A method that can be invoked for a particular channel."""
    def __init__(self, client_impl: 'Impl', rpcs: PendingRpcs,
                 channel: Channel, method: Method,
                 default_timeout_s: Optional[float]):
        self._impl = client_impl
        self._rpcs = rpcs
        self._rpc = PendingRpc(channel, method.service, method)
        self.default_timeout_s: Optional[float] = default_timeout_s

    @property
    def channel(self) -> Channel:
        return self._rpc.channel

    @property
    def method(self) -> Method:
        return self._rpc.method

    @property
    def service(self) -> Service:
        return self._rpc.service

    def invoke(self,
               request: Optional[Message],
               response: ResponseCallback = _default_response,
               completion: CompletionCallback = _default_completion,
               error: ErrorCallback = _default_error,
               *,
               override_pending: bool = True,
               keep_open: bool = False) -> '_AsyncCall':
        """Invokes an RPC with callbacks."""
        self._rpcs.send_request(self._rpc,
                                request,
                                _Callbacks(response, completion, error),
                                override_pending=override_pending,
                                keep_open=keep_open)
        return _AsyncCall(self._rpcs, self._rpc)

    def _send_client_stream(self, request: Message) -> None:
        self._rpcs.send_client_stream(self._rpc, request)

    def _send_client_stream_end(self) -> None:
        self._rpcs.send_client_stream_end(self._rpc)

    def __repr__(self) -> str:
        return self.help()

    def help(self) -> str:
        """Returns a help message about this RPC."""
        function_call = self.method.full_name + '('

        docstring = inspect.getdoc(self.__call__)  # type: ignore[operator] # pylint: disable=no-member
        assert docstring is not None

        annotation = inspect.Signature.from_callable(self).return_annotation  # type: ignore[arg-type] # pylint: disable=line-too-long
        if isinstance(annotation, type):
            annotation = annotation.__name__

        arg_sep = f',\n{" " * len(function_call)}'
        return (
            f'{function_call}'
            f'{arg_sep.join(descriptors.field_help(self.method.request_type))})'
            f'\n\n{textwrap.indent(docstring, "  ")}\n\n'
            f'  Returns {annotation}.')


class RpcTimeout(Exception):
    def __init__(self, rpc: PendingRpc, timeout: Optional[float]):
        super().__init__(
            f'No response received for {rpc.method} after {timeout} s')
        self.rpc = rpc
        self.timeout = timeout


class RpcError(Exception):
    def __init__(self, rpc: PendingRpc, status: Status):
        if status is Status.NOT_FOUND:
            msg = ': the RPC server does not support this RPC'
        else:
            msg = ''

        super().__init__(f'{rpc.method} failed with error {status}{msg}')
        self.rpc = rpc
        self.status = status


class _AsyncCall:
    """Represents an ongoing callback-based call."""

    # TODO(hepler): Consider alternatives (futures) and/or expand functionality.

    def __init__(self, rpcs: PendingRpcs, rpc: PendingRpc):
        self._rpc = rpc
        self._rpcs = rpcs

    def cancel(self) -> bool:
        return self._rpcs.send_cancel(self._rpc)

    def __enter__(self) -> '_AsyncCall':
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.cancel()


class StreamingResponses:
    """Used to iterate over a queue.SimpleQueue."""
    def __init__(self, method_client: _MethodClient,
                 responses: queue.SimpleQueue,
                 default_timeout_s: OptionalTimeout):
        self._method_client = method_client
        self._queue = responses
        self.status: Optional[Status] = None

        if default_timeout_s is UseDefault.VALUE:
            self.default_timeout_s = self._method_client.default_timeout_s
        else:
            self.default_timeout_s = default_timeout_s

    @property
    def method(self) -> Method:
        return self._method_client.method

    def cancel(self) -> None:
        self._method_client._rpcs.send_cancel(self._method_client._rpc)  # pylint: disable=protected-access

    def responses(self,
                  *,
                  block: bool = True,
                  timeout_s: OptionalTimeout = UseDefault.VALUE) -> Iterator:
        """Returns an iterator of stream responses.

        Args:
          timeout_s: timeout in seconds; None blocks indefinitely
        """
        if timeout_s is UseDefault.VALUE:
            timeout_s = self.default_timeout_s

        try:
            while True:
                response = self._queue.get(block, timeout_s)

                if isinstance(response, Exception):
                    raise response

                if isinstance(response, Status):
                    self.status = response
                    return

                yield response
        except queue.Empty:
            self.cancel()
            raise RpcTimeout(self._method_client._rpc, timeout_s)  # pylint: disable=protected-access
        except:
            self.cancel()
            raise

    def __iter__(self):
        return self.responses()

    def __repr__(self) -> str:
        return f'{type(self).__name__}({self.method})'


def _method_client_docstring(method: Method) -> str:
    return f'''\
Class that invokes the {method.full_name} {method.type.sentence_name()} RPC.

Calling this directly invokes the RPC synchronously. The RPC can be invoked
asynchronously using the invoke method.
'''


def _function_docstring(method: Method) -> str:
    return f'''\
Invokes the {method.full_name} {method.type.sentence_name()} RPC.

This function accepts either the request protobuf fields as keyword arguments or
a request protobuf as a positional argument.
'''


def _update_call_method(method: Method, function: Callable) -> None:
    """Updates the name, docstring, and parameters to match a method."""
    function.__name__ = method.full_name
    function.__doc__ = _function_docstring(method)
    _apply_protobuf_signature(method, function)


def _apply_protobuf_signature(method: Method, function: Callable) -> None:
    """Update a function signature to accept proto arguments.

    In order to have good tab completion and help messages, update the function
    signature to accept only keyword arguments for the proto message fields.
    This doesn't actually change the function signature -- it just updates how
    it appears when inspected.
    """
    sig = inspect.signature(function)

    params = [next(iter(sig.parameters.values()))]  # Get the "self" parameter
    params += method.request_parameters()
    params.append(
        inspect.Parameter('pw_rpc_timeout_s', inspect.Parameter.KEYWORD_ONLY))

    function.__signature__ = sig.replace(  # type: ignore[attr-defined]
        parameters=params)


class UnaryResponse(NamedTuple):
    """Result of invoking a unary RPC: status and response."""
    status: Status
    response: Any

    def __repr__(self) -> str:
        return f'({self.status}, {proto_repr(self.response)})'


class _UnaryResponseHandler:
    """Tracks the state of an ongoing synchronous unary RPC call."""
    def __init__(self, rpc: PendingRpc):
        self._rpc = rpc
        self._response: Any = None
        self._status: Optional[Status] = None
        self._error: Optional[RpcError] = None
        self._event = threading.Event()

    def on_response(self, _: PendingRpc, response: Any) -> None:
        self._response = response

    def on_completion(self, _: PendingRpc, status: Status) -> None:
        self._status = status
        self._event.set()

    def on_error(self, _: PendingRpc, status: Status) -> None:
        self._error = RpcError(self._rpc, status)
        self._event.set()

    def wait(self, timeout_s: Optional[float]) -> UnaryResponse:
        if not self._event.wait(timeout_s):
            raise RpcTimeout(self._rpc, timeout_s)

        if self._error is not None:
            raise self._error

        assert self._status is not None
        return UnaryResponse(self._status, self._response)


class ClientStream:
    """Tracks the state of an ongoing client streaming RPC call."""
    def __init__(self, method_client: _MethodClient,
                 default_timeout_s: OptionalTimeout) -> None:
        """Sends out the initial request (no payload) to start the stream."""
        self.status: Optional[Status] = None
        self.response: Any = None
        self.error: Optional[RpcError] = None

        self._method_client = method_client
        self._call: Optional[_AsyncCall] = None
        self._event = threading.Event()

        if default_timeout_s is UseDefault.VALUE:
            self._default_timeout_s = self._method_client.default_timeout_s
        else:
            self._default_timeout_s = default_timeout_s

        self._call = self._method_client.invoke(None, self._on_response,
                                                self._on_completion,
                                                self._on_error)

    def complete(self) -> bool:
        return self.status is not None or self.error is not None

    def send(self, _rpc_request_proto=None, **request_fields) -> None:
        """Sends a message to the server in the client stream."""
        self._method_client._send_client_stream(  # pylint: disable=protected-access
            self._method_client.method.get_request(_rpc_request_proto,
                                                   request_fields))

    def finish_and_wait(self, timeout_s: OptionalTimeout = UseDefault.VALUE):
        """Ends the client stream and waits for a server response."""

        # An error may have already occurred; don't send a CLIENT_STREAM_END if
        # one has.
        if self.error is not None:
            raise self.error

        if timeout_s is UseDefault.VALUE:
            timeout_s = self._default_timeout_s

        self._method_client._send_client_stream_end()  # pylint: disable=protected-access

        self._event.wait(timeout_s)
        assert self.complete()

        if self.error is not None:
            raise self.error

        assert self.status is not None
        return UnaryResponse(self.status, self.response)

    def cancel(self) -> bool:
        if not self._call or not self._call.cancel():
            return False

        self.error = RpcError(self._method_client._rpc, Status.CANCELLED)  # pylint: disable=protected-access
        return True

    def _on_response(self, _: PendingRpc, response: Any) -> None:
        self.response = response

    def _on_completion(self, _: PendingRpc, status: Status) -> None:
        self.status = status
        self._event.set()

    def _on_error(self, rpc: PendingRpc, status: Status) -> None:
        self.error = RpcError(rpc, status)
        self._event.set()

    def __repr__(self) -> str:
        return f'{type(self).__name__}({self._method_client.method})'


_ResponseHandler = Callable[['BidirectionalStream', Any], Any]


class BidirectionalStream(ClientStream):
    """Tracks the state of an ongoing bidirectional streaming RPC call."""
    def __init__(self, method_client: _MethodClient,
                 default_timeout_s: OptionalTimeout,
                 response_handler: _ResponseHandler) -> None:
        """Sends the initial request (no payload) to start the stream."""
        self.response_handler = response_handler
        self.responses: List[Any] = []

        # Invoke the base __init__ after setting member variables since it
        # invokes the RPC.
        super().__init__(method_client, default_timeout_s)

    def _on_response(self, _: PendingRpc, response: Any) -> None:
        # TODO(frolv): self.responses could grow very large for persistent
        # streaming RPCs such as logs. The size of this list should be limited.
        self.responses.append(response)
        self.response = response
        self.response_handler(self, response)

    def __repr__(self) -> str:
        return f'{type(self).__name__}({self._method_client.method})'


def _unary_method_client(client_impl: 'Impl', rpcs: PendingRpcs,
                         channel: Channel, method: Method,
                         default_timeout: Optional[float]) -> _MethodClient:
    """Creates an object used to call a unary method."""
    def call(self: _MethodClient,
             _rpc_request_proto=None,
             *,
             pw_rpc_timeout_s=UseDefault.VALUE,
             **request_fields) -> UnaryResponse:

        handler = _UnaryResponseHandler(self._rpc)  # pylint: disable=protected-access
        self.invoke(
            self.method.get_request(_rpc_request_proto, request_fields),
            handler.on_response, handler.on_completion, handler.on_error)

        if pw_rpc_timeout_s is UseDefault.VALUE:
            pw_rpc_timeout_s = self.default_timeout_s

        return handler.wait(pw_rpc_timeout_s)

    _update_call_method(method, call)

    # The MethodClient class is created dynamically so that the __call__ method
    # can be configured differently for each method.
    method_client_type = type(
        f'{method.name}_UnaryMethodClient', (_MethodClient, ),
        dict(__call__=call, __doc__=_method_client_docstring(method)))
    return method_client_type(client_impl, rpcs, channel, method,
                              default_timeout)


def _server_streaming_method_client(client_impl: 'Impl', rpcs: PendingRpcs,
                                    channel: Channel, method: Method,
                                    default_timeout: Optional[float]):
    """Creates an object used to call a server streaming method."""
    def call(self: _MethodClient,
             _rpc_request_proto=None,
             *,
             pw_rpc_timeout_s=UseDefault.VALUE,
             **request_fields) -> StreamingResponses:
        responses: queue.SimpleQueue = queue.SimpleQueue()
        self.invoke(
            self.method.get_request(_rpc_request_proto, request_fields),
            lambda _, response: responses.put(response),
            lambda _, status: responses.put(status),
            lambda rpc, status: responses.put(RpcError(rpc, status)))
        return StreamingResponses(self, responses, pw_rpc_timeout_s)

    _update_call_method(method, call)

    # The MethodClient class is created dynamically so that the __call__ method
    # can be configured differently for each method type.
    method_client_type = type(
        f'{method.name}_ServerStreamingMethodClient', (_MethodClient, ),
        dict(__call__=call, __doc__=_method_client_docstring(method)))
    return method_client_type(client_impl, rpcs, channel, method,
                              default_timeout)


# Since __call__ doesn't need to be overridden, declare regular classes.
class _ClientStreamingMethodClient(_MethodClient):
    def __call__(
            self,
            pw_rpc_timeout_s: OptionalTimeout = UseDefault.VALUE
    ) -> ClientStream:
        return _create_client_stream(False, self, pw_rpc_timeout_s)


class _BidirectionalStreamingMethodClient(_MethodClient):
    def __call__(
        self,
        response_handler: _ResponseHandler = None,
        pw_rpc_timeout_s: OptionalTimeout = UseDefault.VALUE
    ) -> BidirectionalStream:
        if response_handler is None:
            response_handler = lambda _, rep: _default_response(self._rpc, rep)

        return _create_client_stream(True, self, pw_rpc_timeout_s,
                                     response_handler)


def _create_client_stream(bidirectional: bool, method_client: _MethodClient,
                          pw_rpc_timeout_s: OptionalTimeout, *args,
                          **kwargs) -> BidirectionalStream:
    """Creates the object that represents a client or bidirectional stream."""
    base = BidirectionalStream if bidirectional else ClientStream

    def send(self, _rpc_request_proto=None, **request_fields) -> None:
        ClientStream.send(self, _rpc_request_proto, **request_fields)

    _apply_protobuf_signature(method_client.method, send)

    client_stream_type = type(f'{method_client.method.name}_{base.__name__}',
                              (base, ), dict(send=send))
    return client_stream_type(method_client, pw_rpc_timeout_s, *args, **kwargs)


def _create_method_client(base: type, client_impl: 'Impl', rpcs: PendingRpcs,
                          channel: Channel, method: Method,
                          default_timeout_s: Optional[float]):
    """Creates the method client for a client or bidirectional stream RPC."""
    method_client_type = type(f'{method.name}_{base.__name__}', (base, ),
                              dict(__doc__=_method_client_docstring(method)))
    return method_client_type(client_impl, rpcs, channel, method,
                              default_timeout_s)


class Impl(client.ClientImpl):
    """Callback-based ClientImpl."""
    def __init__(self,
                 default_unary_timeout_s: Optional[float] = 1.0,
                 default_stream_timeout_s: Optional[float] = 1.0):
        super().__init__()
        self._default_unary_timeout_s = default_unary_timeout_s
        self._default_stream_timeout_s = default_stream_timeout_s

    @property
    def default_unary_timeout_s(self) -> Optional[float]:
        return self._default_unary_timeout_s

    @property
    def default_stream_timeout_s(self) -> Optional[float]:
        return self._default_stream_timeout_s

    def method_client(self, channel: Channel, method: Method) -> _MethodClient:
        """Returns an object that invokes a method using the given chanel."""

        if method.type is Method.Type.UNARY:
            return _unary_method_client(self, self.rpcs, channel, method,
                                        self.default_unary_timeout_s)

        if method.type is Method.Type.SERVER_STREAMING:
            return _server_streaming_method_client(
                self, self.rpcs, channel, method,
                self.default_stream_timeout_s)

        if method.type is Method.Type.CLIENT_STREAMING:
            return _create_method_client(_ClientStreamingMethodClient, self,
                                         self.rpcs, channel, method,
                                         self.default_unary_timeout_s)

        if method.type is Method.Type.BIDIRECTIONAL_STREAMING:
            return _create_method_client(_BidirectionalStreamingMethodClient,
                                         self, self.rpcs, channel, method,
                                         self.default_stream_timeout_s)

        raise AssertionError(f'Unknown method type {method.type}')

    def handle_response(self,
                        rpc: PendingRpc,
                        context,
                        payload,
                        *,
                        args: tuple = (),
                        kwargs: dict = None) -> None:
        """Invokes the callback associated with this RPC.

        Any additional positional and keyword args passed through
        Client.process_packet are forwarded to the callback.
        """
        if kwargs is None:
            kwargs = {}

        try:
            context.response(rpc, payload, *args, **kwargs)
        except:  # pylint: disable=bare-except
            self.rpcs.send_cancel(rpc)
            _LOG.exception('Response callback %s for %s raised exception',
                           context.response, rpc)

    def handle_completion(self,
                          rpc: PendingRpc,
                          context,
                          status: Status,
                          *,
                          args: tuple = (),
                          kwargs: dict = None):
        if kwargs is None:
            kwargs = {}

        try:
            context.completion(rpc, status, *args, **kwargs)
        except:  # pylint: disable=bare-except
            _LOG.exception('Completion callback %s for %s raised exception',
                           context.completion, rpc)

    def handle_error(self,
                     rpc: PendingRpc,
                     context,
                     status: Status,
                     *,
                     args: tuple = (),
                     kwargs: dict = None) -> None:
        if kwargs is None:
            kwargs = {}

        try:
            context.error(rpc, status, *args, **kwargs)
        except:  # pylint: disable=bare-except
            _LOG.exception('Error callback %s for %s raised exception',
                           context.error, rpc)
