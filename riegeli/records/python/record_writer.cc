// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// From https://docs.python.org/3/c-api/intro.html:
// Since Python may define some pre-processor definitions which affect the
// standard headers on some systems, you must include Python.h before any
// standard headers are included.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <stddef.h>
#include <string>
#include <utility>

#include "absl/base/optimization.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/python/utils.h"
#include "riegeli/bytes/python/python_writer.h"
#include "riegeli/records/python/record_position.h"
#include "riegeli/records/record_position.h"
#include "riegeli/records/record_writer.h"

namespace riegeli {
namespace python {

namespace {

constexpr ImportedCapsule<RecordPositionApi> kRecordPositionApi(
    kRecordPositionCapsuleName);

PyObject* PyFlushType_Type;

PythonPtr DefineFlushType() {
  static constexpr ImportedConstant kEnum("enum", "Enum");
  if (ABSL_PREDICT_FALSE(!kEnum.Verify())) return nullptr;
  static constexpr Identifier id_FlushType("FlushType");
  const PythonPtr values(Py_BuildValue(
      "((si)(si)(si))", "FROM_OBJECT", static_cast<int>(FlushType::kFromObject),
      "FROM_PROCESS", static_cast<int>(FlushType::kFromProcess), "FROM_MACHINE",
      static_cast<int>(FlushType::kFromMachine)));
  if (ABSL_PREDICT_FALSE(values == nullptr)) return nullptr;
  return PythonPtr(PyObject_CallFunctionObjArgs(kEnum.get(), id_FlushType.get(),
                                                values.get(), nullptr));
}

bool FlushTypeFromPython(PyObject* object, FlushType* value) {
  RIEGELI_ASSERT(PyFlushType_Type != nullptr)
      << "Python FlushType not defined yet";
  if (ABSL_PREDICT_FALSE(!PyObject_IsInstance(object, PyFlushType_Type))) {
    PyErr_Format(PyExc_TypeError, "Expected FlushType, not %s",
                 Py_TYPE(object)->tp_name);
    return false;
  }
  static constexpr Identifier id_value("value");
  const PythonPtr enum_value(PyObject_GetAttr(object, id_value.get()));
  if (ABSL_PREDICT_FALSE(enum_value == nullptr)) return false;
#if PY_MAJOR_VERSION >= 3
  const long long_value = PyLong_AsLong(enum_value.get());
#else
  const long long_value = PyInt_AsLong(enum_value.get());
#endif
  if (ABSL_PREDICT_FALSE(long_value == -1) && PyErr_Occurred()) return false;
  *value = static_cast<FlushType>(long_value);
  return true;
}

class FileDescriptorCollector {
 public:
  bool Init(PyObject* file_descriptors) {
    file_descriptors_ = file_descriptors;
    files_seen_.reset(PySet_New(nullptr));
    return files_seen_ != nullptr;
  }

  bool AddFile(PyObject* file_descriptor) {
    // name = file_descriptor.name
    static constexpr Identifier id_name("name");
    const PythonPtr name(PyObject_GetAttr(file_descriptor, id_name.get()));
    if (ABSL_PREDICT_FALSE(name == nullptr)) return false;
    // if name in self.files_seen: return
    const int contains = PySet_Contains(files_seen_.get(), name.get());
    if (ABSL_PREDICT_FALSE(contains < 0)) return false;
    if (contains != 0) return true;
    // self.files_seen.add(name)
    if (ABSL_PREDICT_FALSE(PySet_Add(files_seen_.get(), name.get()) < 0)) {
      return false;
    }
    // for dependency in file_descriptor.dependencies:
    //   self.add_file(dependency)
    static constexpr Identifier id_dependencies("dependencies");
    const PythonPtr dependencies(
        PyObject_GetAttr(file_descriptor, id_dependencies.get()));
    if (ABSL_PREDICT_FALSE(dependencies == nullptr)) return false;
    const PythonPtr iter(PyObject_GetIter(dependencies.get()));
    if (ABSL_PREDICT_FALSE(iter == nullptr)) return false;
    while (const PythonPtr dependency{PyIter_Next(iter.get())}) {
      if (ABSL_PREDICT_FALSE(!AddFile(dependency.get()))) return false;
    }
    if (ABSL_PREDICT_FALSE(PyErr_Occurred() != nullptr)) return false;
    // file_descriptor_proto = self.file_descriptors.add()
    static constexpr Identifier id_add("add");
    const PythonPtr file_descriptor_proto(
        PyObject_CallMethodObjArgs(file_descriptors_, id_add.get(), nullptr));
    if (ABSL_PREDICT_FALSE(file_descriptor_proto == nullptr)) return false;
    // file_descriptor.CopyToProto(file_descriptor_proto)
    static constexpr Identifier id_CopyToProto("CopyToProto");
    return PythonPtr(PyObject_CallMethodObjArgs(
               file_descriptor, id_CopyToProto.get(),
               file_descriptor_proto.get(), nullptr)) != nullptr;
  }

 private:
  PyObject* file_descriptors_;
  PythonPtr files_seen_;
};

extern "C" PyObject* SetRecordType(PyObject* self, PyObject* args,
                                   PyObject* kwargs) {
  static constexpr const char* keywords[] = {"metadata", "message_type",
                                             nullptr};
  PyObject* metadata_arg;
  PyObject* message_type_arg;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs, "OO:set_record_type", const_cast<char**>(keywords),
          &metadata_arg, &message_type_arg))) {
    return nullptr;
  }
  // message_descriptor = message_type.DESCRIPTOR
  static constexpr Identifier id_DESCRIPTOR("DESCRIPTOR");
  const PythonPtr message_descriptor(
      PyObject_GetAttr(message_type_arg, id_DESCRIPTOR.get()));
  if (ABSL_PREDICT_FALSE(message_descriptor == nullptr)) return nullptr;
  // metadata.record_type_name = message_descriptor.full_name
  static constexpr Identifier id_full_name("full_name");
  const PythonPtr full_name(
      PyObject_GetAttr(message_descriptor.get(), id_full_name.get()));
  if (ABSL_PREDICT_FALSE(full_name == nullptr)) return nullptr;
  static constexpr Identifier id_record_type_name("record_type_name");
  if (ABSL_PREDICT_FALSE(PyObject_SetAttr(metadata_arg,
                                          id_record_type_name.get(),
                                          full_name.get()) < 0)) {
    return nullptr;
  }
  // file_descriptors = metadata.file_descriptor
  static constexpr Identifier id_file_descriptor("file_descriptor");
  const PythonPtr file_descriptors(
      PyObject_GetAttr(metadata_arg, id_file_descriptor.get()));
  if (ABSL_PREDICT_FALSE(file_descriptors == nullptr)) return nullptr;
  // del file_descriptors[:]
  const PythonPtr slice(PySlice_New(nullptr, nullptr, nullptr));
  if (ABSL_PREDICT_FALSE(slice == nullptr)) return nullptr;
  if (ABSL_PREDICT_FALSE(PyObject_DelItem(file_descriptors.get(), slice.get()) <
                         0)) {
    return nullptr;
  }
  // file_descriptor = message_descriptor.file
  static constexpr Identifier id_file("file");
  const PythonPtr file_descriptor(
      PyObject_GetAttr(message_descriptor.get(), id_file.get()));
  if (ABSL_PREDICT_FALSE(file_descriptor == nullptr)) return nullptr;
  // FileDescriptorCollector(file_descriptors).add_file(file_descriptor)
  FileDescriptorCollector collector;
  if (ABSL_PREDICT_FALSE(!collector.Init(file_descriptors.get()))) {
    return nullptr;
  }
  if (ABSL_PREDICT_FALSE(!collector.AddFile(file_descriptor.get()))) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

struct PyRecordWriterObject {
  // clang-format off
  PyObject_HEAD
  static_assert(true, "");  // clang-format workaround.
  // clang-format on

  PythonWrapped<RecordWriter<PythonWriter>> record_writer;
};

extern PyTypeObject PyRecordWriter_Type;

void SetExceptionFromRecordWriter(PyRecordWriterObject* self) {
  RIEGELI_ASSERT(!self->record_writer->healthy())
      << "Failed precondition of SetExceptionFromRecordWriter(): "
         "RecordWriter healthy";
  if (!self->record_writer->dest().exception().ok()) {
    self->record_writer->dest().exception().Restore();
    return;
  }
  SetRiegeliError(self->record_writer->message());
}

extern "C" void RecordWriterDestructor(PyRecordWriterObject* self) {
  PyObject_GC_UnTrack(self);
  PythonUnlocked([&] { self->record_writer.reset(); });
  Py_TYPE(self)->tp_free(self);
}

extern "C" int RecordWriterTraverse(PyRecordWriterObject* self, visitproc visit,
                                    void* arg) {
  if (self->record_writer.has_value()) {
    return self->record_writer->dest().Traverse(visit, arg);
  }
  return 0;
}

extern "C" int RecordWriterClear(PyRecordWriterObject* self) {
  PythonUnlocked([&] { self->record_writer.reset(); });
  return 0;
}

extern "C" int RecordWriterInit(PyRecordWriterObject* self, PyObject* args,
                                PyObject* kwargs) {
  static constexpr const char* keywords[] = {
      "dest",    "close",    "assumed_pos",         "buffer_size",
      "options", "metadata", "serialized_metadata", nullptr};
  PyObject* dest_arg;
  PyObject* close_arg = nullptr;
  PyObject* assumed_pos_arg = nullptr;
  PyObject* buffer_size_arg = nullptr;
  PyObject* options_arg = nullptr;
  PyObject* metadata_arg = nullptr;
  PyObject* serialized_metadata_arg = nullptr;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs,
          "O|"
#if PY_VERSION_HEX >= 0x03030000
          "$"
#endif
          "OOOOOO:RecordWriter",
          const_cast<char**>(keywords), &dest_arg, &close_arg, &assumed_pos_arg,
          &buffer_size_arg, &options_arg, &metadata_arg,
          &serialized_metadata_arg))) {
    return -1;
  }

  PythonWriter::Options python_writer_options;
  if (close_arg != nullptr) {
    const int close_is_true = PyObject_IsTrue(close_arg);
    if (ABSL_PREDICT_FALSE(close_is_true < 0)) return -1;
    python_writer_options.set_close(close_is_true != 0);
  }
  if (assumed_pos_arg != nullptr && assumed_pos_arg != Py_None) {
    Position assumed_pos;
    if (ABSL_PREDICT_FALSE(
            !PositionFromPython(assumed_pos_arg, &assumed_pos))) {
      return -1;
    }
    python_writer_options.set_assumed_pos(assumed_pos);
  }
  if (buffer_size_arg != nullptr) {
    size_t buffer_size;
    if (ABSL_PREDICT_FALSE(!SizeFromPython(buffer_size_arg, &buffer_size))) {
      return -1;
    }
    python_writer_options.set_buffer_size(buffer_size);
  }

  RecordWriterBase::Options record_writer_options;
  if (options_arg != nullptr) {
    TextOrBytes options;
    if (ABSL_PREDICT_FALSE(!options.FromPython(options_arg))) return -1;
    std::string error_message;
    if (ABSL_PREDICT_FALSE(!record_writer_options.FromString(options.data(),
                                                             &error_message))) {
      SetRiegeliError(error_message);
      return -1;
    }
  }
  bool had_metadata = false;
  if (metadata_arg != nullptr && metadata_arg != Py_None) {
    static constexpr Identifier id_SerializeToString("SerializeToString");
    const PythonPtr serialized_metadata_str(PyObject_CallMethodObjArgs(
        metadata_arg, id_SerializeToString.get(), nullptr));
    if (ABSL_PREDICT_FALSE(serialized_metadata_str == nullptr)) return -1;
    Chain serialized_metadata;
    if (ABSL_PREDICT_FALSE(!ChainFromPython(serialized_metadata_str.get(),
                                            &serialized_metadata))) {
      return -1;
    }
    record_writer_options.set_serialized_metadata(
        std::move(serialized_metadata));
    had_metadata = true;
  }
  if (serialized_metadata_arg != nullptr) {
    Chain serialized_metadata;
    if (ABSL_PREDICT_FALSE(
            !ChainFromPython(serialized_metadata_arg, &serialized_metadata))) {
      return -1;
    }
    if (!serialized_metadata.empty()) {
      if (had_metadata) {
        PyErr_SetString(PyExc_TypeError,
                        "RecordWriter() got conflicting keyword arguments "
                        "'metadata' and 'serialized_metadata'");
        return -1;
      }
      record_writer_options.set_serialized_metadata(
          std::move(serialized_metadata));
    }
  }

  PythonWriter python_writer(dest_arg, std::move(python_writer_options));
  PythonUnlocked([&] {
    self->record_writer.emplace(std::move(python_writer),
                                std::move(record_writer_options));
  });
  if (ABSL_PREDICT_FALSE(!self->record_writer->healthy())) {
    self->record_writer->dest().Close();
    SetExceptionFromRecordWriter(self);
    return -1;
  }
  return 0;
}

extern "C" PyObject* RecordWriterDest(PyRecordWriterObject* self,
                                      void* closure) {
  PyObject* const dest = ABSL_PREDICT_FALSE(!self->record_writer.has_value())
                             ? Py_None
                             : self->record_writer->dest().dest();
  Py_INCREF(dest);
  return dest;
}

extern "C" PyObject* RecordWriterRepr(PyRecordWriterObject* self) {
  const PythonPtr format = StringToPython("<RecordWriter dest={!r}>");
  if (ABSL_PREDICT_FALSE(format == nullptr)) return nullptr;
  // return format.format(self.dest)
  PyObject* const dest = ABSL_PREDICT_FALSE(!self->record_writer.has_value())
                             ? Py_None
                             : self->record_writer->dest().dest();
  static constexpr Identifier id_format("format");
  return PyObject_CallMethodObjArgs(format.get(), id_format.get(), dest,
                                    nullptr);
}

extern "C" PyObject* RecordWriterEnter(PyObject* self, PyObject* args) {
  // return self
  Py_INCREF(self);
  return self;
}

extern "C" PyObject* RecordWriterExit(PyRecordWriterObject* self,
                                      PyObject* args) {
  PyObject* exc_type;
  PyObject* exc_value;
  PyObject* traceback;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTuple(args, "OOO:__exit__", &exc_type,
                                           &exc_value, &traceback))) {
    return nullptr;
  }
  // self.close(), suppressing exceptions if exc_type != None.
  if (ABSL_PREDICT_TRUE(self->record_writer.has_value())) {
    const bool ok =
        PythonUnlocked([&] { return self->record_writer->Close(); });
    if (ABSL_PREDICT_FALSE(!ok) && exc_type == Py_None) {
      SetExceptionFromRecordWriter(self);
      return nullptr;
    }
  }
  Py_RETURN_FALSE;
}

extern "C" PyObject* RecordWriterClose(PyRecordWriterObject* self,
                                       PyObject* args) {
  if (ABSL_PREDICT_TRUE(self->record_writer.has_value())) {
    const bool ok =
        PythonUnlocked([&] { return self->record_writer->Close(); });
    if (ABSL_PREDICT_FALSE(!ok)) {
      SetExceptionFromRecordWriter(self);
      return nullptr;
    }
  }
  Py_RETURN_NONE;
}

extern "C" PyObject* RecordWriterWriteRecord(PyRecordWriterObject* self,
                                             PyObject* args, PyObject* kwargs) {
  static constexpr const char* keywords[] = {"record", nullptr};
  PyObject* record_arg;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs, "O:write_record", const_cast<char**>(keywords),
          &record_arg))) {
    return nullptr;
  }
  BytesLike record;
  if (ABSL_PREDICT_FALSE(!record.FromPython(record_arg))) return nullptr;
  if (ABSL_PREDICT_FALSE(!self->record_writer.Verify())) return nullptr;
  const bool ok = PythonUnlocked(
      [&] { return self->record_writer->WriteRecord(record.data()); });
  if (ABSL_PREDICT_FALSE(!ok)) {
    SetExceptionFromRecordWriter(self);
    return nullptr;
  }
  Py_RETURN_NONE;
}

extern "C" PyObject* RecordWriterWriteRecordWithKey(PyRecordWriterObject* self,
                                                    PyObject* args,
                                                    PyObject* kwargs) {
  static constexpr const char* keywords[] = {"record", nullptr};
  PyObject* record_arg;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs, "O:write_record_with_key", const_cast<char**>(keywords),
          &record_arg))) {
    return nullptr;
  }
  BytesLike record;
  if (ABSL_PREDICT_FALSE(!record.FromPython(record_arg))) return nullptr;
  if (ABSL_PREDICT_FALSE(!self->record_writer.Verify())) return nullptr;
  FutureRecordPosition key;
  const bool ok = PythonUnlocked(
      [&] { return self->record_writer->WriteRecord(record.data(), &key); });
  if (ABSL_PREDICT_FALSE(!ok)) {
    SetExceptionFromRecordWriter(self);
    return nullptr;
  }
  if (ABSL_PREDICT_FALSE(!kRecordPositionApi.Verify())) return nullptr;
  return kRecordPositionApi->RecordPositionToPython(std::move(key)).release();
}

extern "C" PyObject* RecordWriterWriteMessage(PyRecordWriterObject* self,
                                              PyObject* args,
                                              PyObject* kwargs) {
  static constexpr const char* keywords[] = {"record", nullptr};
  PyObject* record_arg;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs, "O:write_message", const_cast<char**>(keywords),
          &record_arg))) {
    return nullptr;
  }
  // self.write_record(record.SerializeToString())
  static constexpr Identifier id_SerializeToString("SerializeToString");
  const PythonPtr serialized_object(PyObject_CallMethodObjArgs(
      record_arg, id_SerializeToString.get(), nullptr));
  if (ABSL_PREDICT_FALSE(serialized_object == nullptr)) return nullptr;
  BytesLike serialized;
  if (ABSL_PREDICT_FALSE(!serialized.FromPython(serialized_object.get()))) {
    return nullptr;
  }
  if (ABSL_PREDICT_FALSE(!self->record_writer.Verify())) return nullptr;
  const bool ok = PythonUnlocked(
      [&] { return self->record_writer->WriteRecord(serialized.data()); });
  if (ABSL_PREDICT_FALSE(!ok)) {
    SetExceptionFromRecordWriter(self);
    return nullptr;
  }
  Py_RETURN_NONE;
}

extern "C" PyObject* RecordWriterWriteMessageWithKey(PyRecordWriterObject* self,
                                                     PyObject* args,
                                                     PyObject* kwargs) {
  static constexpr const char* keywords[] = {"record", nullptr};
  PyObject* record_arg;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs, "O:write_message_with_key",
          const_cast<char**>(keywords), &record_arg))) {
    return nullptr;
  }
  // return self.write_record_with_key(record.SerializeToString())
  static constexpr Identifier id_SerializeToString("SerializeToString");
  const PythonPtr serialized_object(PyObject_CallMethodObjArgs(
      record_arg, id_SerializeToString.get(), nullptr));
  if (ABSL_PREDICT_FALSE(serialized_object == nullptr)) return nullptr;
  BytesLike serialized;
  if (ABSL_PREDICT_FALSE(!serialized.FromPython(serialized_object.get()))) {
    return nullptr;
  }
  if (ABSL_PREDICT_FALSE(!self->record_writer.Verify())) return nullptr;
  FutureRecordPosition key;
  const bool ok = PythonUnlocked([&] {
    return self->record_writer->WriteRecord(serialized.data(), &key);
  });
  if (ABSL_PREDICT_FALSE(!ok)) {
    SetExceptionFromRecordWriter(self);
    return nullptr;
  }
  if (ABSL_PREDICT_FALSE(!kRecordPositionApi.Verify())) return nullptr;
  return kRecordPositionApi->RecordPositionToPython(std::move(key)).release();
}

extern "C" PyObject* RecordWriterWriteRecords(PyRecordWriterObject* self,
                                              PyObject* args,
                                              PyObject* kwargs) {
  static constexpr const char* keywords[] = {"records", nullptr};
  PyObject* records_arg;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs, "O:write_records", const_cast<char**>(keywords),
          &records_arg))) {
    return nullptr;
  }
  // for record in records:
  //   self.write_record(record)
  const PythonPtr iter(PyObject_GetIter(records_arg));
  if (ABSL_PREDICT_FALSE(iter == nullptr)) return nullptr;
  while (const PythonPtr record_object{PyIter_Next(iter.get())}) {
    BytesLike record;
    if (ABSL_PREDICT_FALSE(!record.FromPython(record_object.get()))) {
      return nullptr;
    }
    if (ABSL_PREDICT_FALSE(!self->record_writer.Verify())) return nullptr;
    const bool ok = PythonUnlocked(
        [&] { return self->record_writer->WriteRecord(record.data()); });
    if (ABSL_PREDICT_FALSE(!ok)) {
      SetExceptionFromRecordWriter(self);
      return nullptr;
    }
  }
  if (ABSL_PREDICT_FALSE(PyErr_Occurred() != nullptr)) return nullptr;
  Py_RETURN_NONE;
}

extern "C" PyObject* RecordWriterWriteRecordsWithKeys(
    PyRecordWriterObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* keywords[] = {"records", nullptr};
  PyObject* records_arg;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs, "O:write_records_with_keys",
          const_cast<char**>(keywords), &records_arg))) {
    return nullptr;
  }
  // keys = []
  PythonPtr keys(PyList_New(0));
  // for record in records:
  //   keys.append(self.write_record_with_key(record))
  const PythonPtr iter(PyObject_GetIter(records_arg));
  if (ABSL_PREDICT_FALSE(iter == nullptr)) return nullptr;
  while (const PythonPtr record_object{PyIter_Next(iter.get())}) {
    BytesLike record;
    if (ABSL_PREDICT_FALSE(!record.FromPython(record_object.get()))) {
      return nullptr;
    }
    if (ABSL_PREDICT_FALSE(!self->record_writer.Verify())) return nullptr;
    FutureRecordPosition key;
    const bool ok = PythonUnlocked(
        [&] { return self->record_writer->WriteRecord(record.data(), &key); });
    if (ABSL_PREDICT_FALSE(!ok)) {
      SetExceptionFromRecordWriter(self);
      return nullptr;
    }
    if (ABSL_PREDICT_FALSE(!kRecordPositionApi.Verify())) return nullptr;
    const PythonPtr key_object(
        kRecordPositionApi->RecordPositionToPython(std::move(key)));
    if (ABSL_PREDICT_FALSE(key_object == nullptr)) return nullptr;
    if (ABSL_PREDICT_FALSE(PyList_Append(keys.get(), key_object.get()) < 0)) {
      return nullptr;
    }
  }
  if (ABSL_PREDICT_FALSE(PyErr_Occurred() != nullptr)) return nullptr;
  // return keys
  return keys.release();
}

extern "C" PyObject* RecordWriterWriteMessages(PyRecordWriterObject* self,
                                               PyObject* args,
                                               PyObject* kwargs) {
  static constexpr const char* keywords[] = {"records", nullptr};
  PyObject* records_arg;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs, "O:write_messages", const_cast<char**>(keywords),
          &records_arg))) {
    return nullptr;
  }
  // for record in records:
  //   self.write_record(record.SerializeToString())
  const PythonPtr iter(PyObject_GetIter(records_arg));
  if (ABSL_PREDICT_FALSE(iter == nullptr)) return nullptr;
  while (const PythonPtr record_object{PyIter_Next(iter.get())}) {
    static constexpr Identifier id_SerializeToString("SerializeToString");
    const PythonPtr serialized_object(PyObject_CallMethodObjArgs(
        record_object.get(), id_SerializeToString.get(), nullptr));
    if (ABSL_PREDICT_FALSE(serialized_object == nullptr)) return nullptr;
    BytesLike serialized;
    if (ABSL_PREDICT_FALSE(!serialized.FromPython(serialized_object.get()))) {
      return nullptr;
    }
    if (ABSL_PREDICT_FALSE(!self->record_writer.Verify())) return nullptr;
    const bool ok = PythonUnlocked(
        [&] { return self->record_writer->WriteRecord(serialized.data()); });
    if (ABSL_PREDICT_FALSE(!ok)) {
      SetExceptionFromRecordWriter(self);
      return nullptr;
    }
  }
  if (ABSL_PREDICT_FALSE(PyErr_Occurred() != nullptr)) return nullptr;
  Py_RETURN_NONE;
}

extern "C" PyObject* RecordWriterWriteMessagesWithKeys(
    PyRecordWriterObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* keywords[] = {"records", nullptr};
  PyObject* records_arg;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs, "O:write_messages_with_keys",
          const_cast<char**>(keywords), &records_arg))) {
    return nullptr;
  }
  // keys = []
  PythonPtr keys(PyList_New(0));
  // for record in records:
  //   keys.append(self.write_record_with_key(record.SerializeToString()))
  const PythonPtr iter(PyObject_GetIter(records_arg));
  if (ABSL_PREDICT_FALSE(iter == nullptr)) return nullptr;
  while (const PythonPtr record_object{PyIter_Next(iter.get())}) {
    static constexpr Identifier id_SerializeToString("SerializeToString");
    const PythonPtr serialized_object(PyObject_CallMethodObjArgs(
        record_object.get(), id_SerializeToString.get(), nullptr));
    if (ABSL_PREDICT_FALSE(serialized_object == nullptr)) return nullptr;
    BytesLike serialized;
    if (ABSL_PREDICT_FALSE(!serialized.FromPython(serialized_object.get()))) {
      return nullptr;
    }
    if (ABSL_PREDICT_FALSE(!self->record_writer.Verify())) return nullptr;
    FutureRecordPosition key;
    const bool ok = PythonUnlocked([&] {
      return self->record_writer->WriteRecord(serialized.data(), &key);
    });
    if (ABSL_PREDICT_FALSE(!ok)) {
      SetExceptionFromRecordWriter(self);
      return nullptr;
    }
    if (ABSL_PREDICT_FALSE(!kRecordPositionApi.Verify())) return nullptr;
    const PythonPtr key_object(
        kRecordPositionApi->RecordPositionToPython(std::move(key)));
    if (ABSL_PREDICT_FALSE(key_object == nullptr)) return nullptr;
    if (ABSL_PREDICT_FALSE(PyList_Append(keys.get(), key_object.get()) < 0)) {
      return nullptr;
    }
  }
  if (ABSL_PREDICT_FALSE(PyErr_Occurred() != nullptr)) return nullptr;
  // return keys
  return keys.release();
}

extern "C" PyObject* RecordWriterFlush(PyRecordWriterObject* self,
                                       PyObject* args, PyObject* kwargs) {
  static constexpr const char* keywords[] = {"flush_type", nullptr};
  PyObject* flush_type_arg;
  if (ABSL_PREDICT_FALSE(!PyArg_ParseTupleAndKeywords(
          args, kwargs, "O:flush", const_cast<char**>(keywords),
          &flush_type_arg))) {
    return nullptr;
  }
  FlushType flush_type;
  if (ABSL_PREDICT_FALSE(!FlushTypeFromPython(flush_type_arg, &flush_type))) {
    return nullptr;
  }
  if (ABSL_PREDICT_FALSE(!self->record_writer.Verify())) return nullptr;
  const bool ok =
      PythonUnlocked([&] { return self->record_writer->Flush(flush_type); });
  if (ABSL_PREDICT_FALSE(!ok)) {
    SetExceptionFromRecordWriter(self);
    return nullptr;
  }
  Py_RETURN_NONE;
}

extern "C" PyObject* RecordWriterPos(PyRecordWriterObject* self,
                                     void* closure) {
  if (ABSL_PREDICT_FALSE(!self->record_writer.Verify())) return nullptr;
  if (ABSL_PREDICT_FALSE(!kRecordPositionApi.Verify())) return nullptr;
  return kRecordPositionApi->RecordPositionToPython(self->record_writer->Pos())
      .release();
}

const PyMethodDef RecordWriterMethods[] = {
    {"__enter__", RecordWriterEnter, METH_NOARGS,
     R"doc(
__enter__(self) -> RecordWriter

Returns self.
)doc"},
    {"__exit__", reinterpret_cast<PyCFunction>(RecordWriterExit), METH_VARARGS,
     R"doc(
__exit__(self, exc_type, exc_value, traceback) -> bool

Calls close().

Suppresses exceptions from close() if an exception is already in flight.

Args:
  exc_type: None or exception in flight (type).
  exc_value: None or exception in flight (value).
  traceback: None or exception in flight (traceback).
)doc"},
    {"close", reinterpret_cast<PyCFunction>(RecordWriterClose), METH_NOARGS,
     R"doc(
close(self) -> None

Indicates that writing is done.

Writes buffered data to the file. Marks the RecordWriter as closed,
disallowing further writing.

If the RecordWriter was failed, raises the same exception again.

If the RecordWriter was not failed but already closed, does nothing.
)doc"},
    {"write_record", reinterpret_cast<PyCFunction>(RecordWriterWriteRecord),
     METH_VARARGS | METH_KEYWORDS, R"doc(
write_record(self, record: Union[bytes, bytearray, memoryview]) -> None

Writes the next record.

Args:
  record: Record to write as a bytes-like object.
)doc"},
    {"write_record_with_key",
     reinterpret_cast<PyCFunction>(RecordWriterWriteRecordWithKey),
     METH_VARARGS | METH_KEYWORDS, R"doc(
write_record_with_key(
    self, record: Union[bytes, bytearray, memoryview]) -> RecordPosition

Writes the next record.

Args:
  record: Record to write as a bytes-like object.

Returns:
  The canonical record position of the record written.
)doc"},
    {"write_message", reinterpret_cast<PyCFunction>(RecordWriterWriteMessage),
     METH_VARARGS | METH_KEYWORDS, R"doc(
write_message(self, record: Message) -> None

Writes the next record.

Args:
  record: Record to write as a proto message.
)doc"},
    {"write_message_with_key",
     reinterpret_cast<PyCFunction>(RecordWriterWriteMessageWithKey),
     METH_VARARGS | METH_KEYWORDS, R"doc(
write_message_with_key(self, record: Message) -> RecordPosition

Writes the next record.

Args:
  record: Record to write as a proto message.

Returns:
  The canonical record position of the record written.
)doc"},
    {"write_records", reinterpret_cast<PyCFunction>(RecordWriterWriteRecords),
     METH_VARARGS | METH_KEYWORDS, R"doc(
write_records(
    self, records: Iterable[Union[bytes, bytearray, memoryview]]) -> None

Writes a number of records.

Args:
  records: Records to write as an iterable of bytes-like objects.
)doc"},
    {"write_records_with_keys",
     reinterpret_cast<PyCFunction>(RecordWriterWriteRecordsWithKeys),
     METH_VARARGS | METH_KEYWORDS, R"doc(
write_records_with_keys(
    self, records: Iterable[Union[bytes, bytearray, memoryview]]
) -> List[RecordPosition]

Writes a number of records.

Args:
  records: Records to write as an iterable of bytes-like objects.

Returns:
  A list of canonical record positions corresponding to records written.
)doc"},
    {"write_messages", reinterpret_cast<PyCFunction>(RecordWriterWriteMessages),
     METH_VARARGS | METH_KEYWORDS, R"doc(
write_messages(self, records: Iterable[Message]) -> None

Writes a number of records.

Args:
  records: Records to write as an iterable of proto messages.
)doc"},
    {"write_messages_with_keys",
     reinterpret_cast<PyCFunction>(RecordWriterWriteMessagesWithKeys),
     METH_VARARGS | METH_KEYWORDS, R"doc(
write_messages_with_keys(
    self, records: Iterable[Message]) -> List[RecordPosition]

Writes a number of records.

Args:
  records: Records to write as an iterable of proto messages.

Returns:
  A list of canonical record positions corresponding to records written.
)doc"},
    {"flush", reinterpret_cast<PyCFunction>(RecordWriterFlush),
     METH_VARARGS | METH_KEYWORDS, R"doc(
flush(self, flush_type: FlushType) -> None

Finalizes any open chunk and writes buffered data to the file.

If parallelism was used in options, waits for any background writing to
complete.

This degrades compression density if used too often.

Args:
  flush_type: What more to attempt to ensure:
   * FlushType.FROM_OBJECT: Data is written to the file object.
   * FlushType.FROM_PROCESS: Data survives process crash.
   * FlushType.FROM_MACHINE: Data survives operating system crash.
)doc"},
    {nullptr, nullptr, 0, nullptr},
};

const PyGetSetDef RecordWriterGetSet[] = {
    {const_cast<char*>("dest"), reinterpret_cast<getter>(RecordWriterDest),
     nullptr, const_cast<char*>(R"doc(
dest: BinaryIO

Binary IO stream being written to.
)doc"),
     nullptr},
    {const_cast<char*>("pos"), reinterpret_cast<getter>(RecordWriterPos),
     nullptr, const_cast<char*>(R"doc(
pos: RecordPosition

The current position.

pos.numeric returns the position as an int.

A position returned by pos before writing a record is not greater than the
canonical position returned by write_record_with_key() for that record, but
seeking to either position will read the same record.

After close() or flush(), pos is equal to the canonical position returned by the
following write_record_with_key() (after reopening the file for appending in the
case of close()).
)doc"),
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}};

PyTypeObject PyRecordWriter_Type = {
    // clang-format off
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    // clang-format on
    "riegeli.records.python.record_writer."
    "RecordWriter",                                        // tp_name
    sizeof(PyRecordWriterObject),                          // tp_basicsize
    0,                                                     // tp_itemsize
    reinterpret_cast<destructor>(RecordWriterDestructor),  // tp_dealloc
    nullptr,                                               // tp_print
    nullptr,                                               // tp_getattr
    nullptr,                                               // tp_setattr
#if PY_MAJOR_VERSION >= 3
    nullptr,  // tp_as_async
#else
    nullptr,  // tp_compare
#endif
    reinterpret_cast<reprfunc>(RecordWriterRepr),  // tp_repr
    nullptr,                                       // tp_as_number
    nullptr,                                       // tp_as_sequence
    nullptr,                                       // tp_as_mapping
    nullptr,                                       // tp_hash
    nullptr,                                       // tp_call
    nullptr,                                       // tp_str
    nullptr,                                       // tp_getattro
    nullptr,                                       // tp_setattro
    nullptr,                                       // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,  // tp_flags
    R"doc(
RecordWriter(
    dest: BinaryIO,
    *,
    close: bool = True,
    assumed_pos: Optional[int] = None,
    buffer_size: int = 64 << 10,
    options: )doc" RIEGELI_TEXT_OR_BYTES R"doc( = '',
    metadata: Optional[RecordsMetadata] = None,
    serialized_metadata: Union[bytes, bytearray, memoryview] = b''
) -> RecordWriter

Will write to the given file.

Args:
  dest: Binary IO stream to write to.
  close: If True, dest is owned, and close() or __exit__() will call
    dest.close().
  assumed_pos: If None, dest must support random access. If an int, it is enough
    that dest supports sequential access, and this position will be assumed
    initially.
  buffer_size: Tunes how much data is buffered before writing to dest.
  options: Compression and other writing options. See below.
  metadata: If not None, file metadata to be written at the beginning (if
    metadata has any fields set). Metadata are written only when the file is
    written from the beginning, not when it is appended to. Record type in
    metadata can be conveniently set by set_record_type().
  serialized_metadata: If not empty, like metadata, but metadata are passed
    serialized as a bytes-like object. This is faster if the caller has metadata
    already serialized. This conflicts with metadata.

The dest argument should be a binary IO stream which supports:
 * close()          - for close() or __exit__() unless close is False
 * write(bytes)
 * flush()          - for flush(FlushType.FROM_{PROCESS,MACHINE})
 * seek(int[, int]) - unless assumed_pos is not None
 * tell()           - unless assumed_pos is not None

Example values for dest (possibly with 'ab' instead of 'wb' for appending):
 * io.FileIO(filename, 'wb')
 * io.open(filename, 'wb') - better with buffering=0 or use io.FileIO()
 * open(filename, 'wb') - better with buffering=0 or use io.FileIO()
 * io.BytesIO() - use close=False to access dest after closing the RecordWriter
 * tf.gfile.GFile(filename, 'wb')

Syntax of options (same as in C++
riegeli::RecordWriterBase::Options::FromString()):
  options ::= option? ("," option?)*
  option ::=
    "default" |
    "transpose" (":" ("true" | "false"))? |
    "uncompressed" |
    "brotli" (":" brotli_level)? |
    "zstd" (":" zstd_level)? |
    "window_log" ":" window_log |
    "chunk_size" ":" chunk_size |
    "bucket_fraction" ":" bucket_fraction |
    "pad_to_block_boundary" (":" ("true" | "false"))? |
    "parallelism" ":" parallelism
  brotli_level ::= integer 0..11 (default 9)
  zstd_level ::= integer -32..22 (default 9)
  window_log ::= "auto" or integer 10..31
  chunk_size ::=
    integer expressed as real with optional suffix [BkKMGTPE], 1..
  bucket_fraction ::= real 0..1
  parallelism ::= integer 0..

If transpose is true or empty, records should be serialized proto messages (but
nothing will break if they are not). A chunk of records will be processed in a
way which allows for better compression. If transpose is false, a chunk of
records will be stored in a simpler format, directly or with compression.
Default: false.

Supported compression algorithms:
 * uncompressed
 * brotli, compression level defaults to 9
 * zstd, compression level defaults to 9
Default: brotli.

window_log sets the logarithm of the LZ77 sliding window size. This tunes the
tradeoff between compression density and memory usage (higher = better density
but more memory). Special value auto means to keep the default (brotli: 22,
zstd: derived from compression level and chunk size). For uncompressed,
window_log must be auto. For brotli, window_log must be auto or between 10 and
30. For zstd, window_log must be auto or between 10 and 30 in 32-bit build, 31
in 64-bit build. Default: auto.

chunk_size sets the desired uncompressed size of a chunk which groups messages
to be transposed, compressed, and written together. A larger chunk size improves
compression density; a smaller chunk size allows to read pieces of the file
independently with finer granularity, and reduces memory usage of both writer
and reader. Default: 1M.

bucket_fraction sets the desired uncompressed size of a bucket which groups
values of several fields of the given wire type to be compressed together,
relative to the desired chunk size, on the scale between 0.0 (compress each
field separately) to 1.0 (put all fields of the same wire type in the same
bucket. This is meaningful if transpose and compression are enabled. A larger
bucket size improves compression density; a smaller bucket size makes reading
with projection faster, allowing to skip decompression of values of fields which
are not included. Default 1.0.

If pad_to_block_boundary is true or empty, padding is written to reach a 64KB
block boundary when the RecordWriter is created, before close() or __exit__(),
and before flush(). Consequences:
 * Even if the existing file was corrupted or truncated, data appended to it
   will be readable.
 * Physical concatenation of separately written files yields a valid file
   (setting metadata in subsequent files is wasteful but harmless).
 * Up to 64KB is wasted when padding is written.
Default: false.

parallelism sets the maximum number of chunks being encoded in parallel in
background. Larger parallelism can increase throughput, up to a point where it
no longer matters; smaller parallelism reduces memory usage. If parallelism > 0,
chunks are written to dest in background and reporting writing errors is
delayed. Default: 0.
)doc",                                                              // tp_doc
    reinterpret_cast<traverseproc>(RecordWriterTraverse),  // tp_traverse
    reinterpret_cast<inquiry>(RecordWriterClear),          // tp_clear
    nullptr,                                               // tp_richcompare
    0,                                                     // tp_weaklistoffset
    nullptr,                                               // tp_iter
    nullptr,                                               // tp_iternext
    const_cast<PyMethodDef*>(RecordWriterMethods),         // tp_methods
    nullptr,                                               // tp_members
    const_cast<PyGetSetDef*>(RecordWriterGetSet),          // tp_getset
    nullptr,                                               // tp_base
    nullptr,                                               // tp_dict
    nullptr,                                               // tp_descr_get
    nullptr,                                               // tp_descr_set
    0,                                                     // tp_dictoffset
    reinterpret_cast<initproc>(RecordWriterInit),          // tp_init
    nullptr,                                               // tp_alloc
    PyType_GenericNew,                                     // tp_new
    nullptr,                                               // tp_free
    nullptr,                                               // tp_is_gc
    nullptr,                                               // tp_bases
    nullptr,                                               // tp_mro
    nullptr,                                               // tp_cache
    nullptr,                                               // tp_subclasses
    nullptr,                                               // tp_weaklist
    nullptr,                                               // tp_del
    0,                                                     // tp_version_tag
#if PY_VERSION_HEX >= 0x030400a1
    nullptr,  // tp_finalize
#endif
};

const char* const kModuleName = "riegeli.records.python.record_writer";
const char kModuleDoc[] = R"doc(Writes records to a Riegeli/records file.)doc";

const PyMethodDef kModuleMethods[] = {
    {"set_record_type", reinterpret_cast<PyCFunction>(SetRecordType),
     METH_VARARGS | METH_KEYWORDS,
     R"doc(
set_record_type(metadata: RecordsMetadata, message_type: Type[Message]) -> None

Sets record_type_name and file_descriptor in metadata.

Args:
  metadata: Riegeli/records file metadata being filled, typically will become
    the metadata argument of RecordWriter().
  message_type: Promised type of records, typically the argument type of
    RecordWriter.write_message().
)doc"},
    {nullptr, nullptr, 0, nullptr},
};

#if PY_MAJOR_VERSION >= 3
PyModuleDef kModuleDef = {
    PyModuleDef_HEAD_INIT,
    kModuleName,                               // m_name
    kModuleDoc,                                // m_doc
    -1,                                        // m_size
    const_cast<PyMethodDef*>(kModuleMethods),  // m_methods
    nullptr,                                   // m_slots
    nullptr,                                   // m_traverse
    nullptr,                                   // m_clear
    nullptr,                                   // m_free
};
#endif

PyObject* InitModule() {
  if (ABSL_PREDICT_FALSE(PyType_Ready(&PyRecordWriter_Type) < 0)) {
    return nullptr;
  }
#if PY_MAJOR_VERSION >= 3
  PythonPtr module(PyModule_Create(&kModuleDef));
#else
  PythonPtr module(Py_InitModule3(
      kModuleName, const_cast<PyMethodDef*>(kModuleMethods), kModuleDoc));
#endif
  if (ABSL_PREDICT_FALSE(module == nullptr)) return nullptr;
  PyFlushType_Type = DefineFlushType().release();
  if (ABSL_PREDICT_FALSE(PyFlushType_Type == nullptr)) return nullptr;
  if (ABSL_PREDICT_FALSE(PyModule_AddObject(module.get(), "FlushType",
                                            PyFlushType_Type) < 0)) {
    return nullptr;
  }
  Py_INCREF(&PyRecordWriter_Type);
  if (ABSL_PREDICT_FALSE(PyModule_AddObject(module.get(), "RecordWriter",
                                            reinterpret_cast<PyObject*>(
                                                &PyRecordWriter_Type)) < 0)) {
    return nullptr;
  }
  return module.release();
}

}  // namespace

#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC PyInit_record_writer() { return InitModule(); }
#else
PyMODINIT_FUNC initrecord_writer() { InitModule(); }
#endif

}  // namespace python
}  // namespace riegeli