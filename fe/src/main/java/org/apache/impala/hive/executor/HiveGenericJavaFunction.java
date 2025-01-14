// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.impala.hive.executor;

import org.apache.commons.lang.exception.ExceptionUtils;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.hive.metastore.api.Function;
import org.apache.hadoop.hive.metastore.api.FunctionType;
import org.apache.hadoop.hive.metastore.api.PrincipalType;
import org.apache.hadoop.hive.metastore.api.ResourceType;
import org.apache.hadoop.hive.metastore.api.ResourceUri;
import org.apache.hadoop.hive.ql.exec.FunctionUtils.UDFClassType;
import org.apache.hadoop.hive.ql.exec.UDFArgumentException;
import org.apache.hadoop.hive.ql.udf.generic.GenericUDF;
import org.apache.hadoop.hive.serde2.objectinspector.ObjectInspector;
import org.apache.hadoop.hive.serde2.objectinspector.primitive.PrimitiveObjectInspectorFactory;
import org.apache.impala.analysis.FunctionName;
import org.apache.impala.analysis.HdfsUri;
import org.apache.impala.catalog.CatalogException;
import org.apache.impala.catalog.ScalarFunction;
import org.apache.impala.catalog.PrimitiveType;
import org.apache.impala.catalog.ScalarType;
import org.apache.impala.catalog.Type;
import org.apache.impala.common.AnalysisException;
import org.apache.impala.common.FileSystemUtil;
import org.apache.impala.service.BackendConfig;
import org.apache.impala.thrift.TFunction;
import org.apache.impala.thrift.TFunctionBinaryType;

import java.io.IOException;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;

import com.google.common.base.Joiner;
import com.google.common.base.Preconditions;
import com.google.common.collect.Lists;

import org.apache.log4j.Logger;

/**
 * HiveGenericJavaFunction generates the instance of the GenericUDF object given
 * a className.
 */
public class HiveGenericJavaFunction implements HiveJavaFunction {
  private static final Logger LOG = Logger.getLogger(HiveGenericJavaFunction.class);

  private final Function hiveFn_;

  private final Type retType_;

  private final Type[] parameterTypes_;

  private final GenericUDF genericUDF_;

  public HiveGenericJavaFunction(Class<?> udfClass,
      Function hiveFn, Type retType, Type[] parameterTypes)
      throws CatalogException {
    try {
      hiveFn_ = hiveFn;
      retType_ = retType;
      parameterTypes_ = parameterTypes;
      genericUDF_ = createGenericUDFInstance(udfClass);
      checkValidFunction();
    } catch (CatalogException e) {
      String errorMsg = "Error retrieving class " + udfClass + ": " + e.getMessage();
      throw new CatalogException(errorMsg, e);
    }
  }

  public HiveGenericJavaFunction(Class<?> udfClass,
      Type retType, Type[] parameterTypes) throws CatalogException {
    this(udfClass, null, retType, parameterTypes);
  }

  @Override
  public Function getHiveFunction() {
    return hiveFn_;
  }

  /**
   * Currently GenericUDF does not support extracting the parameters and
   * return type out of the method. It is impossible to do via reflection.
   * Potentially this can be done if we add annotations in the class to
   * handle it.
   */
  @Override
  public List<ScalarFunction> extract() throws CatalogException {
    // Return blank list because extraction cannot be done.
    return new ArrayList<>();
  }

  public GenericUDF getGenericUDFInstance() {
    return genericUDF_;
  }

  public Type getRetType() {
    return retType_;
  }

  public Type[] getParameterTypes() {
    return parameterTypes_;
  }

  private GenericUDF createGenericUDFInstance(Class<?> udfClass)
      throws CatalogException {
    try {
      Constructor<?> ctor = udfClass.getConstructor();
      return (GenericUDF) ctor.newInstance();
    } catch (NoSuchMethodException e) {
      throw new CatalogException(
          "Unable to find constructor with no arguments.", e);
    } catch (IllegalArgumentException e) {
      throw new CatalogException(
          "Unable to call UDF constructor with no arguments.", e);
    } catch (InstantiationException|IllegalAccessException|InvocationTargetException e) {
      throw new CatalogException("Unable to call create UDF instance.", e);
    }
  }

  private void checkValidFunction() throws CatalogException {
    try {
      ObjectInspector[] parameterOIs = getInspectors(parameterTypes_);
      // Call the initialize method which will give us the return type that
      // the GenericUDF produces. Then we check if it matches what we expect.
      ObjectInspector returnOI = genericUDF_.initialize(parameterOIs);
      if (returnOI != getInspector(retType_) && !returnOI.getTypeName().equals("void")) {
        throw new CatalogException("Function expected return type " +
            returnOI.getTypeName() + " but was created with " + retType_);
      }
    } catch (UDFArgumentException e) {
      LOG.error(e.getMessage());
      throw new CatalogException("Function cannot be created with the following " +
          "parameters: (" + Joiner.on(",").join(parameterTypes_) + "). ");
    }
  }

  private ObjectInspector[] getInspectors(Type[] typeArray)
      throws CatalogException {
    ObjectInspector[] OIArray = new ObjectInspector[typeArray.length];
    for (int i = 0; i < typeArray.length; ++i) {
      OIArray[i] = getInspector(typeArray[i]);
    }
    return OIArray;
  }

  private ObjectInspector getInspector(Type t) throws CatalogException {
    switch (t.getPrimitiveType().toThrift()) {
      case BOOLEAN:
        return PrimitiveObjectInspectorFactory.writableBooleanObjectInspector;
      case TINYINT:
        return PrimitiveObjectInspectorFactory.writableByteObjectInspector;
      case SMALLINT:
        return PrimitiveObjectInspectorFactory.writableShortObjectInspector;
      case INT:
        return PrimitiveObjectInspectorFactory.writableIntObjectInspector;
      case BIGINT:
        return PrimitiveObjectInspectorFactory.writableLongObjectInspector;
      case FLOAT:
        return PrimitiveObjectInspectorFactory.writableFloatObjectInspector;
      case DOUBLE:
        return PrimitiveObjectInspectorFactory.writableDoubleObjectInspector;
      case STRING:
        return PrimitiveObjectInspectorFactory.writableStringObjectInspector;
      default:
        throw new CatalogException("Unsupported type: " + t);
    }
  }
}
