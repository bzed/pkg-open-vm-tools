/*
 *  Author: bwilliams
 *  Created: April 6, 2012
 *
 *  Copyright (c) 2012 Vmware, Inc.  All rights reserved.
 *  -- VMware Confidential
 *
 *  This code was generated by the script "build/dev/codeGen/genCppDoc". Please
 *  speak to Brian W. before modifying it by hand.
 *
 */

#ifndef CRequestInstanceParameterDoc_h_
#define CRequestInstanceParameterDoc_h_

namespace Caf {

/// A simple container for objects of type RequestInstanceParameter
class CRequestInstanceParameterDoc {
public:
	CRequestInstanceParameterDoc() :
		_isInitialized(false) {}
	virtual ~CRequestInstanceParameterDoc() {}

public:
	/// Initializes the object with everything required by this
	/// container. Once initialized, this object cannot
	/// be changed (i.e. it is immutable).
	void initialize(
		const std::string name,
		const std::string classNamespace,
		const std::string className,
		const std::string classVersion,
		const std::deque<std::string> value) {
		if (! _isInitialized) {
			_name = name;
			_classNamespace = classNamespace;
			_className = className;
			_classVersion = classVersion;
			_value = value;

			_isInitialized = true;
		}
	}

public:
	/// Accessor for the Name
	std::string getName() const {
		return _name;
	}

	/// Accessor for the ClassNamespace
	std::string getClassNamespace() const {
		return _classNamespace;
	}

	/// Accessor for the ClassName
	std::string getClassName() const {
		return _className;
	}

	/// Accessor for the ClassVersion
	std::string getClassVersion() const {
		return _classVersion;
	}

	/// Accessor for the Value
	std::deque<std::string> getValue() const {
		return _value;
	}

private:
	bool _isInitialized;

	std::string _name;
	std::string _classNamespace;
	std::string _className;
	std::string _classVersion;
	std::deque<std::string> _value;

private:
	CAF_CM_DECLARE_NOCOPY(CRequestInstanceParameterDoc);
};

CAF_DECLARE_SMART_POINTER(CRequestInstanceParameterDoc);

}

#endif