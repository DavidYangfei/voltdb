/* This file is part of VoltDB.
 * Copyright (C) 2008-2012 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
 *
 * VoltDB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VoltDB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef HSTOREABSTRACTEXPRESSION_H
#define HSTOREABSTRACTEXPRESSION_H

#include "common/valuevector.h"

#include "boost/shared_ptr.hpp"

#include "json_spirit/json_spirit.h"

namespace voltdb {

class SerializeInput;
class SerializeOutput;
class TableTuple;

/**
 * Predicate objects for filtering tuples during query execution.
 */

// ------------------------------------------------------------------
// AbstractExpression
// Base class for all expression nodes
// ------------------------------------------------------------------
class AbstractExpression {
  public:
    /** destroy this node and all children */
    virtual ~AbstractExpression();

    virtual NValue eval(const TableTuple *tuple1 = NULL, const TableTuple *tuple2 = NULL) const = 0;

    /* debugging methods - some various ways to create a sring
       describing the expression tree */
    std::string debug() const;
    std::string debug(bool traverse) const;
    std::string debug(const std::string &spacer) const;
    virtual std::string debugInfo(const std::string &spacer) const = 0;

    /* serialization methods. expressions are serialized in java and
       deserialized in the execution engine during startup. */

    /** create an expression tree. call this once with the input
        stream positioned at the root expression node */
    static AbstractExpression* buildExpressionTree(json_spirit::Object &obj);

    /** accessors */
    ExpressionType getExpressionType() const {
        return m_type;
    }

    ValueType getValueType() const
    {
        return m_valueType;
    }

    int getValueSize() const
    {
        return m_valueSize;
    }

    // These should really be part of the constructor, but plumbing
    // the type and size args through the whole of the expression world is
    // not something I'm doing right now.
    void setValueType(ValueType type)
    {
        m_valueType = type;
    }

    void setValueSize(int size)
    {
        m_valueSize = size;
    }

    const AbstractExpression *getLeft() const {
        return m_left;
    }

    const AbstractExpression *getRight() const {
        return m_right;
    }

  protected:
    AbstractExpression();
    AbstractExpression(ExpressionType type);
    AbstractExpression(ExpressionType type,
                       AbstractExpression *left,
                       AbstractExpression *right);

  protected:
    AbstractExpression *m_left, *m_right;
    ExpressionType m_type;
    bool m_hasParameter;
    ValueType m_valueType;
    int m_valueSize;
};

}
#endif
