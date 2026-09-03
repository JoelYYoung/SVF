//===- ScalarProjection.h -- Scalar numerical/address projection -*- C++
//-*-===//

#ifndef SVF_AE_SCALAR_PROJECTION_H
#define SVF_AE_SCALAR_PROJECTION_H

#include "Util/GeneralType.h"
#include "Util/SVFUtil.h"

#include <cfloat>
#include <cmath>
#include <sstream>
#include <utility>

#define AddressMask 0x7f000000
#define FlippedAddressMask (AddressMask ^ 0xffffffff)
#define BlackHoleObjAddr 0x7f000000 + 2
#define NullMemAddr 0x7f000000

namespace SVF
{

/**
 * @brief A class representing a bounded 64-bit integer.
 *
 * BoundedInt is a class that represents a 64-bit integer that can also
 * represent positive and negative infinity. It includes a 64-bit integer
 * value and a boolean flag indicating whether the value is infinite.
 * If the value is infinite, the integer value is used to represent the sign
 * of infinity (1 for positive infinity and 0 for negative infinity).
 */
class BoundedInt
{
protected:
    s64_t _iVal; // The 64-bit integer value.
    bool _isInf; // True if the value is infinite. If true, _iVal == 1
    // represents positive infinity and _iVal == 0 represents
    // negative infinity.

    // Default constructor is protected to prevent creating an object without
    // initializing _iVal and _isInf.
    BoundedInt() = default;

public:
    // Constructs a BoundedInt with the given 64-bit integer value. The value is
    // not infinite.
    BoundedInt(s64_t fVal) : _iVal(fVal), _isInf(false) {}

    // Constructs a BoundedInt with the given 64-bit integer value and infinity
    // flag.
    BoundedInt(s64_t fVal, bool isInf) : _iVal(fVal), _isInf(isInf) {}

    // Copy constructor.
    BoundedInt(const BoundedInt& rhs) : _iVal(rhs._iVal), _isInf(rhs._isInf) {}

    // Copy assignment operator.
    BoundedInt& operator=(const BoundedInt& rhs)
    {
        _iVal = rhs._iVal;
        _isInf = rhs._isInf;
        return *this;
    }

    // Move constructor.
    BoundedInt(BoundedInt&& rhs) : _iVal(rhs._iVal), _isInf(rhs._isInf) {}

    // Move assignment operator.
    BoundedInt& operator=(BoundedInt&& rhs)
    {
        _iVal = rhs._iVal;
        _isInf = rhs._isInf;
        return *this;
    }

    // Virtual destructor.
    virtual ~BoundedInt() {}

    // Checks if the BoundedInt represents positive infinity.
    bool is_plus_infinity() const
    {
        return _isInf && _iVal == 1;
    }

    // Checks if the BoundedInt represents negative infinity.
    bool is_minus_infinity() const
    {
        return _isInf && _iVal == -1;
    }

    // Checks if the BoundedInt represents either positive or negative infinity.
    bool is_infinity() const
    {
        return is_plus_infinity() || is_minus_infinity();
    }

    // Sets the BoundedInt to represent positive infinity.
    void set_plus_infinity()
    {
        *this = plus_infinity();
    }

    // Sets the BoundedInt to represent negative infinity.
    void set_minus_infinity()
    {
        *this = minus_infinity();
    }

    // Returns a BoundedInt representing positive infinity.
    static BoundedInt plus_infinity()
    {
        return {1, true};
    }

    // Returns a BoundedInt representing negative infinity.
    static BoundedInt minus_infinity()
    {
        return {-1, true};
    }

    // Checks if the BoundedInt represents zero.
    bool is_zero() const
    {
        return _iVal == 0;
    }

    // Checks if the given BoundedInt represents zero.
    static bool isZero(const BoundedInt& expr)
    {
        return expr._iVal == 0;
    }

    // Checks if the BoundedInt is equal to another BoundedInt.
    bool equal(const BoundedInt& rhs) const
    {
        return _iVal == rhs._iVal && _isInf == rhs._isInf;
    }

    // Checks if the BoundedInt is less than or equal to another BoundedInt.
    bool leq(const BoundedInt& rhs) const
    {
        // If only one of the two BoundedInts is infinite.
        if (is_infinity() ^ rhs.is_infinity())
        {
            if (is_infinity())
            {
                return is_minus_infinity();
            }
            else
            {
                return rhs.is_plus_infinity();
            }
        }
        // If both BoundedInts are infinite.
        if (is_infinity() && rhs.is_infinity())
        {
            if (is_minus_infinity())
                return true;
            else
                return rhs.is_plus_infinity();
        }
        // If neither BoundedInt is infinite.
        else
            return _iVal <= rhs._iVal;
    }

    // Checks if the BoundedInt is greater than or equal to another BoundedInt.
    bool geq(const BoundedInt& rhs) const
    {
        // If only one of the two BoundedInts is infinite.
        if (is_infinity() ^ rhs.is_infinity())
        {
            if (is_infinity())
            {
                return is_plus_infinity();
            }
            else
            {
                return rhs.is_minus_infinity();
            }
        }
        // If both BoundedInts are infinite.
        if (is_infinity() && rhs.is_infinity())
        {
            if (is_plus_infinity())
                return true;
            else
                return rhs.is_minus_infinity();
        }
        // If neither BoundedInt is infinite.
        else
            return _iVal >= rhs._iVal;
    }

    /// Reload operator
    //{%
    // Overloads the equality operator to compare two BoundedInt objects.
    friend bool operator==(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return lhs.equal(rhs);
    }

    // Overloads the inequality operator to compare two BoundedInt objects.
    friend bool operator!=(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return !lhs.equal(rhs);
    }

    // Overloads the greater than operator to compare two BoundedInt objects.
    friend bool operator>(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return !lhs.leq(rhs);
    }

    // Overloads the less than operator to compare two BoundedInt objects.
    friend bool operator<(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return !lhs.geq(rhs);
    }

    // Overloads the less than or equal to operator to compare two BoundedInt
    // objects.
    friend bool operator<=(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return lhs.leq(rhs);
    }

    // Overloads the greater than or equal to operator to compare two BoundedInt
    // objects.
    friend bool operator>=(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return lhs.geq(rhs);
    }

    /**
     * Safely adds two BoundedInt objects.
     *
     * This function adds two BoundedInt objects in a way that respects the
     * bounds of the underlying s64_t type. It checks for conditions that would
     * result in overflow or underflow and returns a representation of positive
     * or negative infinity in those cases. If addition of the two numbers would
     * result in a value that is within the representable range of s64_t, it
     * performs the addition and returns the result. If the addition is not
     * defined (e.g., positive infinity plus negative infinity), it asserts
     * false to indicate an error.
     *
     * @param lhs The first BoundedInt to add. This can be any valid BoundedInt,
     * including positive and negative infinity.
     * @param rhs The second BoundedInt to add. This can be any valid
     * BoundedInt, including positive and negative infinity.
     * @return A BoundedInt that represents the result of the addition. If the
     * addition would result in overflow, the function returns a BoundedInt
     * representing positive infinity. If the addition would result in
     * underflow, the function returns a BoundedInt representing negative
     * infinity. If the addition is not defined (e.g., positive infinity plus
     * negative infinity), the function asserts false and does not return a
     * value.
     */
    static BoundedInt safeAdd(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        // If one number is positive infinity and the other is negative
        // infinity, this is an invalid operation, so we assert false.
        if ((lhs.is_plus_infinity() && rhs.is_minus_infinity()) ||
            (lhs.is_minus_infinity() && rhs.is_plus_infinity()))
        {
            assert(false && "invalid add");
        }

        // If either number is positive infinity, the result is positive
        // infinity.
        if (lhs.is_plus_infinity() || rhs.is_plus_infinity())
        {
            return plus_infinity();
        }

        // If either number is negative infinity, the result is negative
        // infinity.
        if (lhs.is_minus_infinity() || rhs.is_minus_infinity())
        {
            return minus_infinity();
        }

        // If both numbers are positive and their sum would exceed the maximum
        // representable number, the result is positive infinity.
        if (lhs._iVal > 0 && rhs._iVal > 0 &&
            (std::numeric_limits<s64_t>::max() - lhs._iVal) < rhs._iVal)
        {
            return plus_infinity();
        }

        // If both numbers are negative and their sum would be less than the
        // most negative representable number, the result is negative infinity.
        if (lhs._iVal < 0 && rhs._iVal < 0 &&
            (-std::numeric_limits<s64_t>::max() - lhs._iVal) > rhs._iVal)
        {
            return minus_infinity();
        }

        // If none of the above conditions are met, the numbers can be safely
        // added.
        return lhs._iVal + rhs._iVal;
    }

    // Overloads the addition operator to safely add two BoundedInt objects.
    // Utilizes the safeAdd method to handle potential overflow and underflow.
    friend BoundedInt operator+(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return safeAdd(lhs, rhs);
    }

    // Overloads the unary minus operator to negate a BoundedInt object.
    // The operation simply negates the internal integer value.
    friend BoundedInt operator-(const BoundedInt& lhs)
    {
        return {-lhs._iVal, lhs._isInf};
    }

    // Overloads the subtraction operator to safely subtract one BoundedInt
    // object from another. This is implemented as the addition of the lhs and
    // the negation of the rhs, using the safeAdd method for safety.
    friend BoundedInt operator-(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return safeAdd(lhs, -rhs);
    }

    /**
     * @brief Performs safe multiplication of two BoundedInt objects.
     *
     * This function ensures that the multiplication of two BoundedInt objects
     * doesn't result in overflow or underflow. It returns the multiplication
     * result if it can be represented within the range of a 64-bit integer. If
     * the result would be larger than the maximum representable positive
     * number, it returns positive infinity. If the result would be less than
     * the minimum representable negative number, it returns negative infinity.
     * If either of the inputs is zero, the result is zero. If either of the
     * inputs is infinity, the result is determined by the signs of the inputs.
     *
     * @param lhs The first BoundedInt to multiply.
     * @param rhs The second BoundedInt to multiply.
     * @return The result of the multiplication, or positive/negative infinity
     * if the result would be outside the range of a 64-bit integer.
     */
    static BoundedInt safeMul(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        // If either number is zero, the result is zero.
        if (lhs._iVal == 0 || rhs._iVal == 0)
            return 0;

        // If either number is infinity, the result depends on the signs of the
        // numbers.
        if (lhs.is_infinity() || rhs.is_infinity())
        {
            // If the signs of the numbers are the same, the result is positive
            // infinity. If the signs of the numbers are different, the result
            // is negative infinity.
            if (lhs._iVal * rhs._iVal > 0)
            {
                return plus_infinity();
            }
            else
            {
                return minus_infinity();
            }
        }

        // If both numbers are positive and their product would exceed the
        // maximum representable number, the result is positive infinity.
        if (lhs._iVal > 0 && rhs._iVal > 0 &&
            (std::numeric_limits<s64_t>::max() / lhs._iVal) < rhs._iVal)
        {
            return plus_infinity();
        }

        // If both numbers are negative and their product would exceed the
        // maximum representable number, the result is positive infinity.
        if (lhs._iVal < 0 && rhs._iVal < 0 &&
            (std::numeric_limits<s64_t>::max() / lhs._iVal) > rhs._iVal)
        {
            return plus_infinity();
        }

        // If one number is positive and the other is negative and their product
        // would be less than the most negative representable number, the result
        // is negative infinity.
        if ((lhs._iVal > 0 && rhs._iVal < 0 &&
             (-std::numeric_limits<s64_t>::max() / lhs._iVal) > rhs._iVal) ||
            (lhs._iVal < 0 && rhs._iVal > 0 &&
             (-std::numeric_limits<s64_t>::max() / rhs._iVal) > lhs._iVal))
        {
            return minus_infinity();
        }

        // If none of the above conditions are met, the numbers can be safely
        // multiplied.
        return lhs._iVal * rhs._iVal;
    }

    friend BoundedInt operator%(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        if (rhs.is_zero())
            assert(false && "divide by zero");
        else if (!lhs.is_infinity() && !rhs.is_infinity())
            return lhs._iVal % rhs._iVal;
        else if (!lhs.is_infinity() && rhs.is_infinity())
            return 0;
        // TODO: not sure
        else if (lhs.is_infinity() && !rhs.is_infinity())
            return ite(rhs._iVal > 0, lhs, -lhs);
        else
            // TODO: +oo/-oo L'Hôpital's rule?
            return eq(lhs, rhs) ? plus_infinity() : minus_infinity();
        abort();
    }
    // Overloads the multiplication operator to safely multiply two BoundedInt
    // objects. Utilizes the safeMul method to handle potential overflow.
    friend BoundedInt operator*(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return safeMul(lhs, rhs);
    }

    // Overloads the division operator to safely divide a BoundedInt object by
    // another. Utilizes the safeDiv method to handle potential division by zero
    // and overflow.
    friend BoundedInt operator/(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        if (rhs.is_zero())
        {
            assert(false && "divide by zero");
            abort();
        }
        else if (!lhs.is_infinity() && !rhs.is_infinity())
            return lhs._iVal / rhs._iVal;
        else if (!lhs.is_infinity() && rhs.is_infinity())
            return 0;
        else if (lhs.is_infinity() && !rhs.is_infinity())
            return ite(rhs._iVal >= 0, lhs, -lhs);
        else
            return eq(lhs, rhs) ? plus_infinity() : minus_infinity();
    }

    // Overload bitwise operators for BoundedInt objects. These operators
    // directly apply the corresponding bitwise operators to the internal
    // integer values of the BoundedInt objects.

    // Overloads the bitwise XOR operator for BoundedInt objects.
    friend BoundedInt operator^(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return lhs._iVal ^ rhs._iVal;
    }

    // Overloads the bitwise AND operator for BoundedInt objects.
    friend BoundedInt operator&(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return lhs._iVal & rhs._iVal;
    }

    // Overloads the bitwise OR operator for BoundedInt objects.
    friend BoundedInt operator|(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return lhs._iVal | rhs._iVal;
    }

    // Overload logical operators for BoundedInt objects. These operators
    // directly apply the corresponding logical operators to the internal
    // integer values of the BoundedInt objects.

    // Overloads the logical AND operator for BoundedInt objects.
    friend BoundedInt operator&&(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return lhs._iVal && rhs._iVal;
    }

    // Overloads the logical OR operator for BoundedInt objects.
    friend BoundedInt operator||(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return lhs._iVal || rhs._iVal;
    }

    // Overloads the logical NOT operator for BoundedInt objects.
    friend BoundedInt operator!(const BoundedInt& lhs)
    {
        return !lhs._iVal;
    }

    // Overloads the right shift operator for BoundedInt objects.
    // This operation is safe as long as the right-hand side is non-negative.
    // If the left-hand side is zero or infinity, the result is the same as the
    // left-hand side. If the right-hand side is infinity, the result depends on
    // the sign of the left-hand side.
    friend BoundedInt operator>>(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        assert(rhs.geq(0) && "rhs should be greater or equal than 0");
        if (lhs.is_zero())
            return lhs;
        else if (lhs.is_infinity())
            return lhs;
        else if (rhs.is_infinity())
            return lhs.geq(0) ? 0 : -1;
        else
            return lhs._iVal >> rhs._iVal;
    }

    // Overloads the left shift operator for BoundedInt objects.
    // This operation is safe as long as the right-hand side is non-negative.
    // If the left-hand side is zero or infinity, the result is the same as the
    // left-hand side. If the right-hand side is infinity, the result depends on
    // the sign of the left-hand side.
    friend BoundedInt operator<<(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        assert(rhs.geq(0) && "rhs should be greater or equal than 0");
        if (lhs.is_zero())
            return lhs;
        else if (lhs.is_infinity())
            return lhs;
        else if (rhs.is_infinity())
            return lhs.geq(0) ? plus_infinity() : minus_infinity();
        else
            return lhs._iVal << rhs._iVal;
    }

    // Overloads the ternary if-then-else operator for BoundedInt objects.
    // The condition is evaluated as a boolean, and the result is either the
    // second or third argument depending on the condition.
    friend BoundedInt ite(const BoundedInt& cond, const BoundedInt& lhs,
                          const BoundedInt& rhs)
    {
        return cond._iVal != 0 ? lhs : rhs;
    }

    // Overloads the stream insertion operator for BoundedInt objects.
    // This allows BoundedInt objects to be printed directly using std::cout or
    // other output streams.
    friend std::ostream& operator<<(std::ostream& out, const BoundedInt& expr)
    {
        out << expr._iVal;
        return out;
    }

    // Defines a function to compare two BoundedInt objects for equality.
    // This function directly compares the internal integer values of the
    // BoundedInt objects.
    friend bool eq(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        return lhs._iVal == rhs._iVal && lhs._isInf == rhs._isInf;
    }

    // Defines a function to find the minimum of two BoundedInt objects.
    // This function directly compares the internal integer values of the
    // BoundedInt objects, and also checks if either of them represents
    // infinity.
    friend BoundedInt min(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        if (lhs.is_minus_infinity() || rhs.is_minus_infinity())
            return minus_infinity();
        else if (lhs.is_plus_infinity())
            return rhs;
        else if (rhs.is_plus_infinity())
            return lhs;
        else
            return BoundedInt(std::min(lhs._iVal, rhs._iVal));
    }

    // Defines a function to find the maximum of two BoundedInt objects.
    // This function directly compares the internal integer values of the
    // BoundedInt objects, and also checks if either of them represents
    // infinity.
    friend BoundedInt max(const BoundedInt& lhs, const BoundedInt& rhs)
    {
        if (lhs.is_plus_infinity() || rhs.is_plus_infinity())
            return plus_infinity();
        else if (lhs.is_minus_infinity())
            return rhs;
        else if (rhs.is_minus_infinity())
            return lhs;
        else
            return BoundedInt(std::max(lhs._iVal, rhs._iVal));
    }

    // Defines a function to find the minimum of a vector of BoundedInt objects.
    // This function iterates over the vector and returns the smallest
    // BoundedInt object.
    static BoundedInt min(std::vector<BoundedInt>& _l)
    {
        BoundedInt ret(plus_infinity());
        for (const auto& it : _l)
        {
            if (it.is_minus_infinity())
                return minus_infinity();
            else if (!it.geq(ret))
            {
                ret = it;
            }
        }
        return ret;
    }

    // Defines a function to find the maximum of a vector of BoundedInt objects.
    // This function iterates over the vector and returns the largest BoundedInt
    // object.
    static BoundedInt max(std::vector<BoundedInt>& _l)
    {
        BoundedInt ret(minus_infinity());
        for (const auto& it : _l)
        {
            if (it.is_plus_infinity())
                return plus_infinity();
            else if (!it.leq(ret))
            {
                ret = it;
            }
        }
        return ret;
    }

    // Defines a function to find the absolute value of a BoundedInt object.
    // This function directly applies the unary minus operator if the BoundedInt
    // object is negative.
    friend BoundedInt abs(const BoundedInt& lhs)
    {
        return lhs.leq(0) ? -lhs : lhs;
    }

    // Defines a method to check if a BoundedInt object is true.
    // A BoundedInt object is considered true if its internal integer value is
    // non-zero.
    inline bool is_true() const
    {
        return _iVal != 0;
    }

    /**
     * @brief Retrieves the numeral value of the BoundedInt object.
     *
     * This method returns the numeral representation of the BoundedInt object.
     * If the object represents negative infinity, it returns the minimum
     * representable 64-bit integer. If the object represents positive infinity,
     * it returns the maximum representable 64-bit integer. Otherwise, it
     * returns the actual 64-bit integer value of the object.
     *
     * @return The numeral value of the BoundedInt object.
     */
    inline s64_t getNumeral() const
    {
        // If the object represents negative infinity, return the minimum
        // representable 64-bit integer.
        if (is_minus_infinity())
        {
            return std::numeric_limits<s64_t>::min();
        }
        // If the object represents positive infinity, return the maximum
        // representable 64-bit integer.
        else if (is_plus_infinity())
        {
            return std::numeric_limits<s64_t>::max();
        }
        // Otherwise, return the actual 64-bit integer value of the object.
        else
        {
            return _iVal;
        }
    }

    inline virtual const std::string to_string() const
    {
        if (is_minus_infinity())
        {
            return "-oo";
        }
        if (is_plus_infinity())
        {
            return "+oo";
        }
        else
            return std::to_string(_iVal);
    }

    //%}

    bool is_real() const
    {
        return false;
    }

    inline s64_t getIntNumeral() const
    {
        return getNumeral();
    }

    inline double getRealNumeral() const
    {
        assert(false && "cannot get real number for integer!");
        abort();
    }

    const double getFVal() const
    {
        assert(false && "cannot get real number for integer!");
        abort();
    }
};
/*!
 * Bounded double numeric value
 */
class BoundedDouble
{
protected:
    double _fVal;

    BoundedDouble() = default;

public:
    BoundedDouble(double fVal) : _fVal(fVal) {}

    BoundedDouble(const BoundedDouble& rhs) : _fVal(rhs._fVal) {}

    BoundedDouble& operator=(const BoundedDouble& rhs)
    {
        _fVal = rhs._fVal;
        return *this;
    }

    BoundedDouble(BoundedDouble&& rhs) : _fVal(std::move(rhs._fVal)) {}

    BoundedDouble& operator=(BoundedDouble&& rhs)
    {
        _fVal = std::move(rhs._fVal);
        return *this;
    }

    virtual ~BoundedDouble() {}

    static bool doubleEqual(double a, double b)
    {
        if (std::isinf(a) && std::isinf(b))
            return a == b;
        return std::fabs(a - b) < std::numeric_limits<double>::epsilon();
    }

    const double getFVal() const
    {
        return _fVal;
    }

    bool is_plus_infinity() const
    {
        return _fVal == std::numeric_limits<double>::infinity();
    }

    bool is_minus_infinity() const
    {
        return _fVal == -std::numeric_limits<double>::infinity();
    }

    bool is_infinity() const
    {
        return is_plus_infinity() || is_minus_infinity();
    }

    void set_plus_infinity()
    {
        *this = plus_infinity();
    }

    void set_minus_infinity()
    {
        *this = minus_infinity();
    }

    static BoundedDouble plus_infinity()
    {
        return std::numeric_limits<double>::infinity();
    }

    static BoundedDouble minus_infinity()
    {
        return -std::numeric_limits<double>::infinity();
    }

    bool is_zero() const
    {
        return doubleEqual(_fVal, 0.0f);
    }

    static bool isZero(const BoundedDouble& expr)
    {
        return doubleEqual(expr.getFVal(), 0.0f);
    }

    bool equal(const BoundedDouble& rhs) const
    {
        return doubleEqual(_fVal, rhs._fVal);
    }

    bool leq(const BoundedDouble& rhs) const
    {
        if (is_infinity() ^ rhs.is_infinity())
        {
            if (is_infinity())
            {
                return is_minus_infinity();
            }
            else
            {
                return rhs.is_plus_infinity();
            }
        }
        if (is_infinity() && rhs.is_infinity())
        {
            if (is_minus_infinity())
                return true;
            else
                return rhs.is_plus_infinity();
        }
        else
            return _fVal <= rhs._fVal;
    }

    bool geq(const BoundedDouble& rhs) const
    {
        if (is_infinity() ^ rhs.is_infinity())
        {
            if (is_infinity())
            {
                return is_plus_infinity();
            }
            else
            {
                return rhs.is_minus_infinity();
            }
        }
        if (is_infinity() && rhs.is_infinity())
        {
            if (is_plus_infinity())
                return true;
            else
                return rhs.is_minus_infinity();
        }
        else
            return _fVal >= rhs._fVal;
    }

    /// Reload operator
    //{%
    friend bool operator==(const BoundedDouble& lhs, const BoundedDouble& rhs)
    {
        return lhs.equal(rhs);
    }

    friend bool operator!=(const BoundedDouble& lhs, const BoundedDouble& rhs)
    {
        return !lhs.equal(rhs);
    }

    friend bool operator>(const BoundedDouble& lhs, const BoundedDouble& rhs)
    {
        return !lhs.leq(rhs);
    }

    friend bool operator<(const BoundedDouble& lhs, const BoundedDouble& rhs)
    {
        return !lhs.geq(rhs);
    }

    friend bool operator<=(const BoundedDouble& lhs, const BoundedDouble& rhs)
    {
        return lhs.leq(rhs);
    }

    friend bool operator>=(const BoundedDouble& lhs, const BoundedDouble& rhs)
    {
        return lhs.geq(rhs);
    }

    /**
     * Adds two floating-point numbers safely, checking for overflow and
     * underflow conditions.
     *
     * @param lhs Left-hand side operand of the addition.
     * @param rhs Right-hand side operand of the addition.
     * @return The sum of lhs and rhs. If overflow or underflow occurs, returns
     * positive or negative infinity.
     */
    static double safeAdd(double lhs, double rhs)
    {
        if ((lhs == std::numeric_limits<double>::infinity() &&
             rhs == -std::numeric_limits<double>::infinity()) ||
            (lhs == -std::numeric_limits<double>::infinity() &&
             rhs == std::numeric_limits<double>::infinity()))
        {
            assert(false && "invalid add");
        }
        double res =
            lhs + rhs; // Perform the addition and store the result in 'res'

        // Check if the result is positive infinity due to overflow
        if (res == std::numeric_limits<double>::infinity())
        {
            return res; // Positive overflow has occurred, return positive
            // infinity
        }

        // Check if the result is negative infinity, which can indicate a large
        // negative overflow
        if (res == -std::numeric_limits<double>::infinity())
        {
            return res; // Negative "overflow", effectively an underflow to
            // negative infinity
        }

        // Check for positive overflow: verify if both operands are positive and
        // their sum exceeds the maximum double value
        if (lhs > 0 && rhs > 0 &&
            (std::numeric_limits<double>::max() - lhs) < rhs)
        {
            res = std::numeric_limits<double>::infinity(); // Set result to
            // positive infinity to
            // indicate overflow
            return res;
        }

        // Check for an underflow scenario: both numbers are negative and their
        // sum is more negative than what double can represent
        if (lhs < 0 && rhs < 0 &&
            (-std::numeric_limits<double>::max() - lhs) > rhs)
        {
            res = -std::numeric_limits<
                double>::infinity(); // Set result to negative infinity to
            // clarify extreme negative sum
            return res;
        }

        // If none of the above conditions are met, return the result of the
        // addition
        return res;
    }

    friend BoundedDouble operator+(const BoundedDouble& lhs,
                                   const BoundedDouble& rhs)
    {
        return safeAdd(lhs._fVal, rhs._fVal);
    }

    friend BoundedDouble operator-(const BoundedDouble& lhs)
    {
        return -lhs._fVal;
    }

    friend BoundedDouble operator-(const BoundedDouble& lhs,
                                   const BoundedDouble& rhs)
    {
        return safeAdd(lhs._fVal, -rhs._fVal);
    }

    /**
     * Safely multiplies two floating-point numbers, checking for overflow and
     * underflow.
     *
     * @param lhs Left-hand side operand of the multiplication.
     * @param rhs Right-hand side operand of the multiplication.
     * @return The product of lhs and rhs. If overflow or underflow occurs,
     * returns positive or negative infinity accordingly.
     */
    static double safeMul(double lhs, double rhs)
    {
        if (doubleEqual(lhs, 0.0f) || doubleEqual(rhs, 0.0f))
            return 0.0f;
        double res = lhs * rhs;
        // Check if the result is positive infinity due to overflow
        if (res == std::numeric_limits<double>::infinity())
        {
            return res; // Positive overflow has occurred, return positive
            // infinity
        }

        // Check if the result is negative infinity, which can indicate a large
        // negative overflow
        if (res == -std::numeric_limits<double>::infinity())
        {
            return res; // Negative "overflow", effectively an underflow to
            // negative infinity
        }
        // Check for overflow scenarios
        if (lhs > 0 && rhs > 0 &&
            lhs > std::numeric_limits<double>::max() / rhs)
        {
            return std::numeric_limits<double>::infinity();
        }
        if (lhs < 0 && rhs < 0 &&
            lhs < std::numeric_limits<double>::max() / rhs)
        {
            return std::numeric_limits<double>::infinity();
        }

        // Check for "underflow" scenarios (negative overflow)
        if (lhs > 0 && rhs < 0 &&
            rhs < std::numeric_limits<double>::lowest() / lhs)
        {
            return -std::numeric_limits<double>::infinity();
        }
        if (lhs < 0 && rhs > 0 &&
            lhs < std::numeric_limits<double>::lowest() / rhs)
        {
            return -std::numeric_limits<double>::infinity();
        }

        return res; // If no overflow or underflow, return the product
    }

    friend BoundedDouble operator*(const BoundedDouble& lhs,
                                   const BoundedDouble& rhs)
    {
        return safeMul(lhs._fVal, rhs._fVal);
    }

    /**
     * Safely divides one floating-point number by another, checking for
     * division by zero and overflow.
     *
     * @param lhs Left-hand side operand (numerator).
     * @param rhs Right-hand side operand (denominator).
     * @return The quotient of lhs and rhs. Returns positive or negative
     * infinity for division by zero, or when overflow occurs.
     */
    static double safeDiv(double lhs, double rhs)
    {
        // Check for division by zero
        if (doubleEqual(rhs, 0.0f))
        {
            return (lhs >= 0.0f) ? std::numeric_limits<double>::infinity()
                                 : -std::numeric_limits<double>::infinity();
        }
        double res = lhs / rhs;
        // Check if the result is positive infinity due to overflow
        if (res == std::numeric_limits<double>::infinity())
        {
            return res; // Positive overflow has occurred, return positive
            // infinity
        }

        // Check if the result is negative infinity, which can indicate a large
        // negative overflow
        if (res == -std::numeric_limits<double>::infinity())
        {
            return res; // Negative "overflow", effectively an underflow to
            // negative infinity
        }

        // Check for overflow when dividing small numbers
        if (rhs > 0 && rhs < std::numeric_limits<double>::min() &&
            lhs > std::numeric_limits<double>::max() * rhs)
        {
            return std::numeric_limits<double>::infinity();
        }
        if (rhs < 0 && rhs > -std::numeric_limits<double>::min() &&
            lhs > std::numeric_limits<double>::max() * rhs)
        {
            return -std::numeric_limits<double>::infinity();
        }

        return res; // If no special cases, return the quotient
    }

    friend BoundedDouble operator/(const BoundedDouble& lhs,
                                   const BoundedDouble& rhs)
    {
        return safeDiv(lhs._fVal, rhs._fVal);
    }

    friend BoundedDouble operator%(const BoundedDouble& lhs,
                                   const BoundedDouble& rhs)
    {
        if (rhs.is_zero())
            assert(false && "divide by zero");
        else if (!lhs.is_infinity() && !rhs.is_infinity())
            return std::fmod(lhs._fVal, rhs._fVal);
        else if (!lhs.is_infinity() && rhs.is_infinity())
            return 0.0f;
        // TODO: not sure
        else if (lhs.is_infinity() && !rhs.is_infinity())
            return ite(rhs._fVal > 0.0f, lhs, -lhs);
        else
            // TODO: +oo/-oo L'Hôpital's rule?
            return eq(lhs, rhs) ? plus_infinity() : minus_infinity();
        abort();
    }

    inline bool is_int() const
    {
        return _fVal == std::round(_fVal);
    }
    inline bool is_real() const
    {
        return !is_int();
    }

    friend BoundedDouble operator^(const BoundedDouble& lhs,
                                   const BoundedDouble& rhs)
    {
        int lInt = std::round(lhs._fVal), rInt = std::round(rhs._fVal);
        return lInt ^ rInt;
    }

    friend BoundedDouble operator&(const BoundedDouble& lhs,
                                   const BoundedDouble& rhs)
    {
        int lInt = std::round(lhs._fVal), rInt = std::round(rhs._fVal);
        return lInt & rInt;
    }

    friend BoundedDouble operator|(const BoundedDouble& lhs,
                                   const BoundedDouble& rhs)
    {
        int lInt = std::round(lhs._fVal), rInt = std::round(rhs._fVal);
        return lInt | rInt;
    }

    friend BoundedDouble operator&&(const BoundedDouble& lhs,
                                    const BoundedDouble& rhs)
    {
        return lhs._fVal && rhs._fVal;
    }

    friend BoundedDouble operator||(const BoundedDouble& lhs,
                                    const BoundedDouble& rhs)
    {
        return lhs._fVal || rhs._fVal;
    }

    friend BoundedDouble operator!(const BoundedDouble& lhs)
    {
        return !lhs._fVal;
    }

    friend BoundedDouble operator>>(const BoundedDouble& lhs,
                                    const BoundedDouble& rhs)
    {
        assert(rhs.geq(0) && "rhs should be greater or equal than 0");
        if (lhs.is_zero())
            return lhs;
        else if (lhs.is_infinity())
            return lhs;
        else if (rhs.is_infinity())
            return lhs.geq(0) ? 0 : -1;
        else
            return (s32_t)lhs.getNumeral() >> (s32_t)rhs.getNumeral();
    }

    friend BoundedDouble operator<<(const BoundedDouble& lhs,
                                    const BoundedDouble& rhs)
    {
        assert(rhs.geq(0) && "rhs should be greater or equal than 0");
        if (lhs.is_zero())
            return lhs;
        else if (lhs.is_infinity())
            return lhs;
        else if (rhs.is_infinity())
            return lhs.geq(0) ? plus_infinity() : minus_infinity();
        else
            return (s32_t)lhs.getNumeral() << (s32_t)rhs.getNumeral();
    }

    friend BoundedDouble ite(const BoundedDouble& cond,
                             const BoundedDouble& lhs, const BoundedDouble& rhs)
    {
        return cond._fVal != 0.0f ? lhs._fVal : rhs._fVal;
    }

    friend std::ostream& operator<<(std::ostream& out,
                                    const BoundedDouble& expr)
    {
        out << expr._fVal;
        return out;
    }

    friend bool eq(const BoundedDouble& lhs, const BoundedDouble& rhs)
    {
        return doubleEqual(lhs._fVal, rhs._fVal);
    }

    friend BoundedDouble min(const BoundedDouble& lhs, const BoundedDouble& rhs)
    {
        return std::min(lhs._fVal, rhs._fVal);
    }

    friend BoundedDouble max(const BoundedDouble& lhs, const BoundedDouble& rhs)
    {
        return std::max(lhs._fVal, rhs._fVal);
    }

    static BoundedDouble min(std::vector<BoundedDouble>& _l)
    {
        BoundedDouble ret(plus_infinity());
        for (const auto& it : _l)
        {
            if (it.is_minus_infinity())
                return minus_infinity();
            else if (!it.geq(ret))
            {
                ret = it;
            }
        }
        return ret;
    }

    static BoundedDouble max(std::vector<BoundedDouble>& _l)
    {
        BoundedDouble ret(minus_infinity());
        for (const auto& it : _l)
        {
            if (it.is_plus_infinity())
                return plus_infinity();
            else if (!it.leq(ret))
            {
                ret = it;
            }
        }
        return ret;
    }

    friend BoundedDouble abs(const BoundedDouble& lhs)
    {
        return lhs.leq(0) ? -lhs : lhs;
    }

    inline bool is_true() const
    {
        return _fVal != 0.0f;
    }

    /// Return Numeral
    inline s64_t getNumeral() const
    {
        if (is_minus_infinity())
        {
            return INT64_MIN;
        }
        else if (is_plus_infinity())
        {
            return INT64_MAX;
        }
        else
        {
            return std::round(_fVal);
        }
    }

    inline s64_t getIntNumeral() const
    {
        return getNumeral();
    }

    inline double getRealNumeral() const
    {
        return _fVal;
    }

    inline virtual const std::string to_string() const
    {
        return std::to_string(_fVal);
    }

    //%}
}; // end class BoundedDouble

/// IntegerIntervalProjection abstract value
///
/// Implemented as a pair of bounds
class IntegerIntervalProjection
{
private:
    // Lower bound
    BoundedInt _lb;

    // Upper bound
    BoundedInt _ub;

    // Invariant: isBottom() <=> _lb = +inf && _ub = -inf
public:
    friend IntegerIntervalProjection operator+(
        const IntegerIntervalProjection& lhs,
        const IntegerIntervalProjection& rhs);
    friend IntegerIntervalProjection operator-(
        const IntegerIntervalProjection& lhs,
        const IntegerIntervalProjection& rhs);
    friend IntegerIntervalProjection operator*(
        const IntegerIntervalProjection& lhs,
        const IntegerIntervalProjection& rhs);
    friend IntegerIntervalProjection operator/(
        const IntegerIntervalProjection& lhs,
        const IntegerIntervalProjection& rhs);
    friend IntegerIntervalProjection operator<<(
        const IntegerIntervalProjection& lhs,
        const IntegerIntervalProjection& rhs);
    friend IntegerIntervalProjection operator>>(
        const IntegerIntervalProjection& lhs,
        const IntegerIntervalProjection& rhs);
    friend IntegerIntervalProjection operator&(
        const IntegerIntervalProjection& lhs,
        const IntegerIntervalProjection& rhs);
    friend IntegerIntervalProjection operator|(
        const IntegerIntervalProjection& lhs,
        const IntegerIntervalProjection& rhs);
    friend IntegerIntervalProjection operator^(
        const IntegerIntervalProjection& lhs,
        const IntegerIntervalProjection& rhs);

    bool isTop() const
    {
        return _lb.is_minus_infinity() && _ub.is_plus_infinity();
    }

    bool isBottom() const
    {
        return _lb.is_plus_infinity() && _ub.is_minus_infinity();
    }

    /// Get minus infinity -inf
    static BoundedInt minus_infinity()
    {
        return BoundedInt::minus_infinity();
    }

    /// Get plus infinity +inf
    static BoundedInt plus_infinity()
    {
        return BoundedInt::plus_infinity();
    }

    static bool is_infinite(const BoundedInt& e)
    {
        return e.is_infinity();
    }

    /// Create the IntegerIntervalProjection [-inf, +inf]
    static IntegerIntervalProjection top()
    {
        return IntegerIntervalProjection(minus_infinity(), plus_infinity());
    }

    /// Create the bottom IntegerIntervalProjection [+inf, -inf]
    static IntegerIntervalProjection bottom()
    {
        return IntegerIntervalProjection(plus_infinity(), minus_infinity());
    }

    /// Create default IntegerIntervalProjection
    explicit IntegerIntervalProjection()
        : _lb(minus_infinity()), _ub(plus_infinity())
    {
    }

    /// Create the IntegerIntervalProjection [n, n]
    explicit IntegerIntervalProjection(s64_t n) : _lb(n), _ub(n) {}

    explicit IntegerIntervalProjection(s32_t n)
        : IntegerIntervalProjection((s64_t)n)
    {
    }

    explicit IntegerIntervalProjection(u32_t n)
        : IntegerIntervalProjection((s64_t)n)
    {
    }

    explicit IntegerIntervalProjection(double n) : _lb(n), _ub(n) {}

    explicit IntegerIntervalProjection(BoundedInt n)
        : IntegerIntervalProjection(n, n)
    {
    }

    /// Create the IntegerIntervalProjection [lb, ub]
    explicit IntegerIntervalProjection(BoundedInt lb, BoundedInt ub)
        : _lb(std::move(lb)), _ub(std::move(ub))
    {
        assert((isBottom() || _lb.leq(_ub)) &&
               "lower bound should be less than or equal to upper bound");
    }

    explicit IntegerIntervalProjection(s64_t lb, s64_t ub)
        : IntegerIntervalProjection(BoundedInt(lb), BoundedInt(ub))
    {
    }

    explicit IntegerIntervalProjection(double lb, double ub)
        : IntegerIntervalProjection(BoundedInt(lb), BoundedInt(ub))
    {
    }

    explicit IntegerIntervalProjection(float lb, float ub)
        : IntegerIntervalProjection(BoundedInt(lb), BoundedInt(ub))
    {
    }

    explicit IntegerIntervalProjection(s32_t lb, s32_t ub)
        : IntegerIntervalProjection((s64_t)lb, (s64_t)ub)
    {
    }

    explicit IntegerIntervalProjection(u32_t lb, u32_t ub)
        : IntegerIntervalProjection((s64_t)lb, (s64_t)ub)
    {
    }

    explicit IntegerIntervalProjection(u64_t lb, u64_t ub)
        : IntegerIntervalProjection((s64_t)lb, (s64_t)ub)
    {
    }

    /// Copy constructor
    IntegerIntervalProjection(const IntegerIntervalProjection&) = default;

    /// Move constructor
    IntegerIntervalProjection(IntegerIntervalProjection&&) = default;

    /// Copy assignment operator
    IntegerIntervalProjection& operator=(const IntegerIntervalProjection& a) =
        default;

    /// Move assignment operator
    IntegerIntervalProjection& operator=(IntegerIntervalProjection&&) = default;

    /// Equality comparison
    IntegerIntervalProjection operator==(
        const IntegerIntervalProjection& other) const
    {
        if (this->isBottom() || other.isBottom())
        {
            return bottom();
        }
        else if (this->isTop() || other.isTop())
        {
            return top();
        }
        else
        {
            if (this->is_numeral() && other.is_numeral())
            {
                return eq(this->_lb, other._lb)
                           ? IntegerIntervalProjection(1, 1)
                           : IntegerIntervalProjection(0, 0);
            }
            else
            {
                IntegerIntervalProjection value = *this;
                value.meet_with(other);
                if (value.isBottom())
                {
                    return IntegerIntervalProjection(0, 0);
                }
                else
                {
                    // return top();
                    return IntegerIntervalProjection(0, 1);
                }
            }
        }
    }

    /// Equality comparison
    IntegerIntervalProjection operator!=(
        const IntegerIntervalProjection& other) const
    {
        if (this->isBottom() || other.isBottom())
        {
            return bottom();
        }
        else if (this->isTop() || other.isTop())
        {
            return top();
        }
        else
        {
            if (this->is_numeral() && other.is_numeral())
            {
                return eq(this->_lb, other._lb)
                           ? IntegerIntervalProjection(0, 0)
                           : IntegerIntervalProjection(1, 1);
            }
            else
            {
                IntegerIntervalProjection value = *this;
                value.meet_with(other);
                if (!value.isBottom())
                {
                    return IntegerIntervalProjection(0, 1);
                }
                else
                {
                    return IntegerIntervalProjection(1, 1);
                }
            }
        }
    }

    /// Destructor
    ~IntegerIntervalProjection() = default;

    /// Return the lower bound
    const BoundedInt& lb() const
    {
        assert(!this->isBottom() &&
               "bottom interval does not have lower bound");
        return this->_lb;
    }

    /// Return the upper bound
    const BoundedInt& ub() const
    {
        assert(!this->isBottom() &&
               "bottom interval does not have upper bound");
        return this->_ub;
    }

    /// Return true if the IntegerIntervalProjection is [0, 0]
    bool is_zero() const
    {
        return _lb.is_zero() && _ub.is_zero();
    }

    /// Return true if the IntegerIntervalProjection is infinite
    /// IntegerIntervalProjection
    bool is_infinite() const
    {
        return _lb.is_infinity() || _ub.is_infinity();
    }

    bool is_int() const
    {
        return !is_real();
    }

    bool is_real() const
    {
        bool lb_real = _lb.is_real();
        bool ub_real = _ub.is_real();
        return lb_real || ub_real;
    }

    /// Return
    s64_t getNumeral() const
    {
        assert(is_numeral() && "this IntegerIntervalProjection is not numeral");
        return _lb.getNumeral();
    }

    s64_t getIntNumeral() const
    {
        assert(is_numeral() && "this IntegerIntervalProjection is not numeral");
        return _lb.getIntNumeral();
    }

    double getRealNumeral() const
    {
        assert(is_numeral() && "this IntegerIntervalProjection is not numeral");
        return _lb.getRealNumeral();
    }

    /// Return true if the IntegerIntervalProjection is a number [num, num]
    bool is_numeral() const
    {
        return eq(_lb, _ub);
    }

    /// Set current IntegerIntervalProjection as bottom
    void set_to_bottom()
    {
        this->_lb = plus_infinity();
        this->_ub = minus_infinity();
    }

    /// Set current IntegerIntervalProjection as top
    void set_to_top()
    {
        this->_lb = minus_infinity();
        this->_ub = plus_infinity();
    }

    /// Determines if the current IntegerIntervalProjection is fully contained
    /// within another IntegerIntervalProjection. Example: this: [2, 3], other:
    /// [1, 4] -> returns true Note: If the current interval is 'bottom', it is
    /// considered contained within any interval.
    ///       If the other interval is 'bottom', it cannot contain any interval.
    bool containedWithin(const IntegerIntervalProjection& other) const
    {
        if (this->isBottom())
        {
            return true;
        }
        else if (other.isBottom())
        {
            return false;
        }
        else
        {
            return other._lb.leq(this->_lb) && this->_ub.leq(other._ub);
        }
    }

    /// Determines if the current IntegerIntervalProjection fully contains
    /// another IntegerIntervalProjection. Example: this: [1, 4], other: [2, 3]
    /// -> returns true Note: If the current interval is 'bottom', it is
    /// considered to contain any interval.
    ///       If the other interval is 'bottom', it cannot be contained by any
    ///       interval.
    bool contain(const IntegerIntervalProjection& other) const
    {
        if (this->isBottom())
        {
            return true;
        }
        else if (other.isBottom())
        {
            return false;
        }
        else
        {
            return other._lb.geq(this->_lb) && this->_ub.geq(other._ub);
        }
    }

    /// Check the upper bound of this Interval is less than or equal to the
    /// lower bound e.g. [1, 3] < [3, 5] return true, lhs.ub <= rhs.lb
    bool leq(const IntegerIntervalProjection& other) const
    {
        if (this->isBottom())
        {
            return true;
        }
        else if (other.isBottom())
        {
            return false;
        }
        else
        {
            return this->_ub.leq(other._lb);
        }
    }

    /// Check the lower bound of this Interval is greater than or equal to the
    /// upper bound e.g. [3, 5] > [1, 3] return true, lhs.lb >= rhs.ub
    bool geq(const IntegerIntervalProjection& other) const
    {
        if (this->isBottom())
        {
            return true;
        }
        else if (other.isBottom())
        {
            return false;
        }
        else
        {
            return this->_lb.geq(other._ub);
        }
    }

    /// Equality comparison
    bool equals(const IntegerIntervalProjection& other) const
    {
        if (this->isBottom())
        {
            return other.isBottom();
        }
        else if (other.isBottom())
        {
            return false;
        }
        else
        {
            if (this->is_real() && other.is_real())
            {
                return this->_lb.equal(other._lb) && this->_ub.equal(other._ub);
            }
            else if (this->is_int() && other.is_int())
            {
                return this->_lb.equal(other._lb) && this->_ub.equal(other._ub);
            }
            else if (this->is_int())
            {
                double thislb = this->_lb.getIntNumeral();
                double thisub = this->_ub.getIntNumeral();
                double otherlb = other._lb.getRealNumeral();
                double otherub = other._ub.getRealNumeral();
                return thislb == otherlb && thisub == otherub;
            }
            else
            {
                double thislb = this->_lb.getRealNumeral();
                double thisub = this->_ub.getRealNumeral();
                double otherlb = other._lb.getIntNumeral();
                double otherub = other._ub.getIntNumeral();
                return thislb == otherlb && thisub == otherub;
            }
            assert(false && "not implemented");
        }
    }

    /// Current IntegerIntervalProjection joins with another
    /// IntegerIntervalProjection
    void join_with(const IntegerIntervalProjection& other)
    {
        if (this->isBottom())
        {
            if (other.isBottom())
            {
                return;
            }
            else
            {
                setValue(other.lb(), other.ub());
            }
        }
        else if (other.isBottom())
        {
            return;
        }
        else
        {
            setValue(min(this->lb(), other.lb()), max(this->ub(), other.ub()));
        }
    }

    /// Current IntegerIntervalProjection widen with another
    /// IntegerIntervalProjection
    void widen_with(const IntegerIntervalProjection& other)
    {
        if (this->isBottom())
        {
            this->_lb = other._lb;
            this->_ub = other._ub;
        }
        else if (other.isBottom())
        {
            return;
        }
        else
        {
            setValue(!lb().leq(other.lb()) ? minus_infinity() : this->lb(),
                     !ub().geq(other.ub()) ? plus_infinity() : this->ub());
        }
    }

    /// Current IntegerIntervalProjection narrow with another
    /// IntegerIntervalProjection
    void narrow_with(const IntegerIntervalProjection& other)
    {
        if (this->isBottom() || other.isBottom())
        {
            this->set_to_bottom();
        }
        else if (other.isBottom())
        {
            return;
        }
        else
        {
            setValue(is_infinite(this->lb()) ? other._lb : this->_lb,
                     is_infinite(this->ub()) ? other._ub : this->_ub);
        }
    }

    /// Return a intersected IntegerIntervalProjection
    void meet_with(const IntegerIntervalProjection& other)
    {
        if (this->isBottom() || other.isBottom())
        {
            this->set_to_bottom();
        }
        else
        {
            if (!(max(this->_lb, other.lb()).leq(min(this->_ub, other.ub()))))
            {
                this->set_to_bottom();
            }
            else
            {
                setValue(max(this->_lb, other.lb()),
                         min(this->_ub, other.ub()));
            }
        }
    }

    /// Return true if the IntegerIntervalProjection contains n
    bool contains(int n) const
    {
        return this->_lb.leq(n) && this->_ub.geq(n);
    }

    void dump(std::ostream& o) const
    {
        if (this->isBottom())
        {
            o << "⊥";
        }
        else
        {
            o << "[" << this->_lb << ", " << this->_ub << "]";
        }
    }

    const std::string toString() const
    {
        std::string str;
        std::stringstream rawStr(str);
        if (this->isBottom())
        {
            rawStr << "⊥";
        }
        else
        {
            rawStr << "[" << lb().to_string() << ", " << ub().to_string()
                   << "]";
        }
        return rawStr.str();
    }

private:
    /// Set the lower bound
    void setValue(const BoundedInt& lb, const BoundedInt& ub)
    {
        assert((isBottom() || _lb.leq(_ub)) &&
               "lower bound should be less than or equal to upper bound");
        this->_lb = lb;
        this->_ub = ub;
    }

private:
    // internal use for create bottom-tolerant IntegerIntervalProjection
    static IntegerIntervalProjection create(const BoundedInt& lb,
                                            const BoundedInt& ub)
    {
        if (!lb.leq(ub))
            return IntegerIntervalProjection::bottom();
        else
            return IntegerIntervalProjection(lb, ub);
    }
}; // end class IntegerIntervalProjection

/// Add integer interval projections.
inline IntegerIntervalProjection operator+(const IntegerIntervalProjection& lhs,
                                           const IntegerIntervalProjection& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
    {
        return IntegerIntervalProjection::bottom();
    }
    else if (lhs.isTop() || rhs.isTop())
    {
        return IntegerIntervalProjection::top();
    }
    else
    {
        return IntegerIntervalProjection(lhs.lb() + rhs.lb(),
                                         lhs.ub() + rhs.ub());
    }
}

/// Subtract integer interval projections.
inline IntegerIntervalProjection operator-(const IntegerIntervalProjection& lhs,
                                           const IntegerIntervalProjection& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
    {
        return IntegerIntervalProjection::bottom();
    }
    else if (lhs.isTop() || rhs.isTop())
    {
        return IntegerIntervalProjection::top();
    }
    else
    {
        return IntegerIntervalProjection(lhs.lb() - rhs.ub(),
                                         lhs.ub() - rhs.lb());
    }
}

/// Multiply integer interval projections.
inline IntegerIntervalProjection operator*(const IntegerIntervalProjection& lhs,
                                           const IntegerIntervalProjection& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
    {
        return IntegerIntervalProjection::bottom();
    }
    else
    {
        BoundedInt ll = lhs.lb() * rhs.lb();
        BoundedInt lu = lhs.lb() * rhs.ub();
        BoundedInt ul = lhs.ub() * rhs.lb();
        BoundedInt uu = lhs.ub() * rhs.ub();
        std::vector<BoundedInt> vec{ll, lu, ul, uu};
        return IntegerIntervalProjection(BoundedInt::min(vec),
                                         BoundedInt::max(vec));
    }
}

/// Divide integer interval projections.
inline IntegerIntervalProjection operator/(const IntegerIntervalProjection& lhs,
                                           const IntegerIntervalProjection& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
    {
        return IntegerIntervalProjection::bottom();
    }
    else if (rhs.contains(0))
    {
        IntegerIntervalProjection lb =
            IntegerIntervalProjection::create(rhs.lb(), -1);
        IntegerIntervalProjection ub =
            IntegerIntervalProjection::create(1, rhs.ub());
        IntegerIntervalProjection l_res = lhs / lb;
        IntegerIntervalProjection r_res = lhs / ub;
        l_res.join_with(r_res);
        return l_res;
    }
    else if (lhs.contains(0))
    {
        IntegerIntervalProjection lb =
            IntegerIntervalProjection::create(lhs.lb(), -1);
        IntegerIntervalProjection ub =
            IntegerIntervalProjection::create(1, lhs.ub());
        IntegerIntervalProjection l_res = lb / rhs;
        IntegerIntervalProjection r_res = ub / rhs;
        l_res.join_with(r_res);
        l_res.join_with(IntegerIntervalProjection(0));
        return l_res;
    }
    else
    {
        // Neither the dividend nor the divisor contains 0
        BoundedInt ll = lhs.lb() / rhs.lb();
        BoundedInt lu = lhs.lb() / rhs.ub();
        BoundedInt ul = lhs.ub() / rhs.lb();
        BoundedInt uu = lhs.ub() / rhs.ub();
        std::vector<BoundedInt> vec{ll, lu, ul, uu};

        IntegerIntervalProjection res = IntegerIntervalProjection(
            BoundedInt::min(vec), BoundedInt::max(vec));
        return res;
    }
}

/// Divide integer interval projections.
inline IntegerIntervalProjection operator%(const IntegerIntervalProjection& lhs,
                                           const IntegerIntervalProjection& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
    {
        return IntegerIntervalProjection::bottom();
    }
    else if (rhs.contains(0))
    {
        return lhs.is_zero() ? IntegerIntervalProjection(0, 0)
                             : IntegerIntervalProjection::top();
    }
    else if (lhs.is_numeral() && rhs.is_numeral())
    {
        return IntegerIntervalProjection(lhs.lb() % rhs.lb(),
                                         lhs.lb() % rhs.lb());
    }
    else
    {
        BoundedInt n_ub = max(abs(lhs.lb()), abs(lhs.ub()));
        BoundedInt d_ub = max(abs(rhs.lb()), rhs.ub()) - 1;
        BoundedInt ub = min(n_ub, d_ub);

        if (lhs.lb().getNumeral() < 0)
        {
            if (lhs.ub().getNumeral() > 0)
            {
                return IntegerIntervalProjection(-ub, ub);
            }
            else
            {
                return IntegerIntervalProjection(-ub, 0);
            }
        }
        else
        {
            return IntegerIntervalProjection(0, ub);
        }
    }
}

// Compare two integer interval projections for greater than.
inline IntegerIntervalProjection operator>(const IntegerIntervalProjection& lhs,
                                           const IntegerIntervalProjection& rhs)
{
    // If either lhs or rhs is bottom, the result is bottom
    if (lhs.isBottom() || rhs.isBottom())
    {
        return IntegerIntervalProjection::bottom();
    }
    // If either lhs or rhs is top, the result is top
    else if (lhs.isTop() || rhs.isTop())
    {
        return IntegerIntervalProjection::top();
    }
    else
    {
        // If both lhs and rhs are numerals (lb = ub), directly compare their
        // values
        if (lhs.is_numeral() && rhs.is_numeral())
        {
            // It means lhs.lb() > rhs.lb()? true: false
            return lhs.lb().leq(rhs.lb()) ? IntegerIntervalProjection(0, 0)
                                          : IntegerIntervalProjection(1, 1);
        }
        else
        {
            // Return [1,1] means lhs is totally greater than rhs
            // When lhs.lb > rhs.ub, e.g., lhs:[3, 4] rhs:[1, 2]
            // lhs.lb(3) > rhs.ub(2)
            // It means lhs.lb() > rhs.ub()
            if (!lhs.lb().leq(rhs.ub()))
            {
                return IntegerIntervalProjection(1, 1);
            }
            // Return [0,0] means lhs is totally impossible to be greater than
            // rhs i.e., lhs is totally less than or equal to rhs When lhs.ub <=
            // rhs.lb, e.g., lhs:[3, 4] rhs:[4，5] lhs.ub(4) <= rhs.lb(4)
            else if (lhs.ub().leq(rhs.lb()))
            {
                return IntegerIntervalProjection(0, 0);
            }
            // For other cases, lhs can be greater than or not, depending on the
            // values e.g., lhs: [2,4], rhs: [1,3], lhs can be greater than rhs
            // if lhs is 4 and rhs is 1. lhs can also not be greater than rhs if
            // lhs is 2 and rhs is 3
            else
            {
                return IntegerIntervalProjection(0, 1);
            }
        }
    }
}

// Compare two integer interval projections for less than.
inline IntegerIntervalProjection operator<(const IntegerIntervalProjection& lhs,
                                           const IntegerIntervalProjection& rhs)
{
    // If either lhs or rhs is bottom, the result is bottom
    if (lhs.isBottom() || rhs.isBottom())
    {
        return IntegerIntervalProjection::bottom();
    }
    // If either lhs or rhs is top, the result is top
    else if (lhs.isTop() || rhs.isTop())
    {
        return IntegerIntervalProjection::top();
    }
    else
    {
        // If both lhs and rhs are numerals (lb = ub), directly compare their
        // values
        if (lhs.is_numeral() && rhs.is_numeral())
        {
            // It means lhs.lb() < rhs.lb()? true: false
            return lhs.lb().geq(rhs.lb()) ? IntegerIntervalProjection(0, 0)
                                          : IntegerIntervalProjection(1, 1);
        }
        else
        {
            // Return [1,1] means lhs is totally less than rhs
            // When lhs.ub < rhs.lb, e.g., lhs:[1, 2] rhs:[3, 4]
            // lhs.ub(2) < rhs.lb(3)
            // It means lhs.ub() < rhs.lb()
            if (!lhs.ub().geq(rhs.lb()))
            {
                return IntegerIntervalProjection(1, 1);
            }
            // Return [0,0] means lhs is totally impossible to be less than rhs
            // i.e., lhs is totally greater than or equal to rhs
            // When lhs.lb >= rhs.ub, e.g., lhs:[4,5] rhs:[3，4]
            // lhs.lb(4) >= rhs.ub(4)
            else if (lhs.lb().geq(rhs.ub()))
            {
                return IntegerIntervalProjection(0, 0);
            }
            // For other cases, lhs can be less than rhs or not, depending on
            // the values e.g., lhs: [2,4], rhs: [1,3], lhs can be less than rhs
            // if lhs is 2, rhs is 3. lhs can also not be less than rhs if lhs
            // is 4 and rhs is 1
            else
            {
                return IntegerIntervalProjection(0, 1);
            }
        }
    }
}

// Compare two integer interval projections for greater than or equal to.
inline IntegerIntervalProjection operator>=(
    const IntegerIntervalProjection& lhs, const IntegerIntervalProjection& rhs)
{
    // If either lhs or rhs is bottom, the result is bottom
    if (lhs.isBottom() || rhs.isBottom())
    {
        return IntegerIntervalProjection::bottom();
    }
    // If either lhs or rhs is top, the result is top
    else if (lhs.isTop() || rhs.isTop())
    {
        return IntegerIntervalProjection::top();
    }
    else
    {
        // If both lhs and rhs are numerals (lb = ub), directly compare their
        // values
        if (lhs.is_numeral() && rhs.is_numeral())
        {
            return lhs.lb().geq(rhs.lb()) ? IntegerIntervalProjection(1, 1)
                                          : IntegerIntervalProjection(0, 0);
        }
        else
        {
            // Return [1,1] means lhs is totally greater than or equal to rhs
            // When lhs.lb >= rhs.ub, e.g., lhs:[2, 3] rhs:[1, 2]
            // lhs.lb(2) >= rhs.ub(2)
            if (lhs.lb().geq(rhs.ub()))
            {
                return IntegerIntervalProjection(1, 1);
            }
            // Return [0,0] means lhs is totally impossible to be greater than
            // or equal to rhs i.e., lhs is totally less than rhs When lhs.ub <
            // rhs.lb, e.g., lhs:[1, 2] rhs:[3, 4] lhs.ub(2) < rhs.lb(3) It
            // means lhs.ub() < rhs.lb()
            else if (!lhs.ub().geq(rhs.lb()))
            {
                return IntegerIntervalProjection(0, 0);
            }
            // For other cases, lhs can be greater than or equal to rhs or not,
            // depending on the values e.g., lhs: [2,4], rhs: [1,3], lhs can be
            // greater than or equal to rhs if lhs is 3, rhs is 2. lhs can also
            // not be greater than or equal to rhs if lhs is 2 and rhs is 3
            else
            {
                return IntegerIntervalProjection(0, 1);
            }
        }
    }
}

// Compare two integer interval projections for less than or equal to.
inline IntegerIntervalProjection operator<=(
    const IntegerIntervalProjection& lhs, const IntegerIntervalProjection& rhs)
{
    // If either lhs or rhs is bottom, the result is bottom
    if (lhs.isBottom() || rhs.isBottom())
    {
        return IntegerIntervalProjection::bottom();
    }
    // If either lhs or rhs is top, the result is top
    else if (lhs.isTop() || rhs.isTop())
    {
        return IntegerIntervalProjection::top();
    }
    else
    {
        // If both lhs and rhs are numerals (lb = ub), directly compare their
        // values
        if (lhs.is_numeral() && rhs.is_numeral())
        {
            return lhs.lb().leq(rhs.lb()) ? IntegerIntervalProjection(1, 1)
                                          : IntegerIntervalProjection(0, 0);
        }
        else
        {
            // Return [1,1] means lhs is totally less than or equal to rhs
            // When lhs.ub <= rhs.lb, e.g., lhs:[1, 2] rhs:[2, 3]
            // lhs.ub(2) <= rhs.lb(2)
            if (lhs.ub().leq(rhs.lb()))
            {
                return IntegerIntervalProjection(1, 1);
            }
            // Return [0,0] means lhs is totally impossible to be less than or
            // equal to rhs i.e., lhs is totally greater than rhs When lhs.lb >
            // rhs.ub, e.g., lhs:[3, 4] rhs:[1, 2] lhs.lb(3) > rhs.ub(2) It
            // means lhs.lb() > rhs.ub()
            else if (!lhs.lb().leq(rhs.ub()))
            {
                return IntegerIntervalProjection(0, 0);
            }
            // For other cases, lhs can be less than or equal to rhs or not,
            // depending on the values e.g., lhs: [2,4], rhs: [1,3], lhs can be
            // less than or equal to rhs if lhs is 3, rhs is 3. lhs can also not
            // be less than or equal to rhs if lhs is 3 and rhs is 2
            else
            {
                return IntegerIntervalProjection(0, 1);
            }
        }
    }
}

/// Left binary shift of integer interval projections.
inline IntegerIntervalProjection operator<<(
    const IntegerIntervalProjection& lhs, const IntegerIntervalProjection& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return IntegerIntervalProjection::bottom();
    if (lhs.isTop() && rhs.isTop())
        return IntegerIntervalProjection::top();
    else
    {
        IntegerIntervalProjection shift = rhs;
        shift.meet_with(IntegerIntervalProjection(
            0, IntegerIntervalProjection::plus_infinity()));
        if (shift.isBottom())
            return IntegerIntervalProjection::bottom();
        if (shift.lb().is_infinity() || shift.ub().is_infinity())
            return IntegerIntervalProjection::top();
        const s64_t lowerShift = shift.lb().getNumeral();
        const s64_t upperShift = shift.ub().getNumeral();
        // 2^63 is not representable by the signed numeral carrier.  Keep the
        // operation sound instead of overflowing a host integer while
        // constructing the scaling interval.
        if (lowerShift >= 63 || upperShift >= 63)
            return IntegerIntervalProjection::top();
        const BoundedInt lb = s64_t{1} << lowerShift;
        const BoundedInt ub = s64_t{1} << upperShift;
        IntegerIntervalProjection coeff(lb, ub);
        return lhs * coeff;
    }
}

/// Left binary shift of integer interval projections.
inline IntegerIntervalProjection operator>>(
    const IntegerIntervalProjection& lhs, const IntegerIntervalProjection& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return IntegerIntervalProjection::bottom();
    else if (lhs.isTop() && rhs.isTop())
        return IntegerIntervalProjection::top();
    else
    {
        IntegerIntervalProjection shift = rhs;
        shift.meet_with(IntegerIntervalProjection(
            0, IntegerIntervalProjection::plus_infinity()));
        if (shift.isBottom())
            return IntegerIntervalProjection::bottom();
        if (lhs.contains(0))
        {
            IntegerIntervalProjection l =
                IntegerIntervalProjection::create(lhs.lb(), -1);
            IntegerIntervalProjection u =
                IntegerIntervalProjection::create(1, lhs.ub());
            IntegerIntervalProjection tmp = l >> rhs;
            tmp.join_with(u >> rhs);
            tmp.join_with(IntegerIntervalProjection(0));
            return tmp;
        }
        else
        {
            BoundedInt ll = lhs.lb() >> shift.lb();
            BoundedInt lu = lhs.lb() >> shift.ub();
            BoundedInt ul = lhs.ub() >> shift.lb();
            BoundedInt uu = lhs.ub() >> shift.ub();
            std::vector<BoundedInt> vec{ll, lu, ul, uu};
            return IntegerIntervalProjection(BoundedInt::min(vec),
                                             BoundedInt::max(vec));
        }
    }
}

/// Bitwise AND of integer interval projections.
inline IntegerIntervalProjection operator&(const IntegerIntervalProjection& lhs,
                                           const IntegerIntervalProjection& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return IntegerIntervalProjection::bottom();
    else if (lhs.is_numeral() && rhs.is_numeral())
    {
        return IntegerIntervalProjection(lhs.lb() & rhs.lb());
    }
    else if (lhs.lb().getNumeral() >= 0 && rhs.lb().getNumeral() >= 0)
    {
        return IntegerIntervalProjection((s64_t)0, min(lhs.ub(), rhs.ub()));
    }
    else if (lhs.lb().getNumeral() >= 0)
    {
        return IntegerIntervalProjection((s64_t)0, lhs.ub());
    }
    else if (rhs.lb().getNumeral() >= 0)
    {
        return IntegerIntervalProjection((s64_t)0, rhs.ub());
    }
    else
    {
        return IntegerIntervalProjection::top();
    }
}

/// Bitwise OR of integer interval projections.
inline IntegerIntervalProjection operator|(const IntegerIntervalProjection& lhs,
                                           const IntegerIntervalProjection& rhs)
{
    auto next_power_of_2 = [](s64_t num) {
        int i = 1;
        while ((num >> i) != 0)
        {
            ++i;
        }
        return 1 << i;
    };
    if (lhs.isBottom() || rhs.isBottom())
        return IntegerIntervalProjection::bottom();
    else if (lhs.is_numeral() && rhs.is_numeral())
        return IntegerIntervalProjection(lhs.lb() | rhs.lb());
    else if (lhs.lb().getNumeral() >= 0 && !lhs.ub().is_infinity() &&
             rhs.lb().getNumeral() >= 0 && !rhs.ub().is_infinity())
    {
        s64_t m = std::max(lhs.ub().getNumeral(), rhs.ub().getNumeral());
        s64_t ub = next_power_of_2(s64_t(m)) - 1;
        return IntegerIntervalProjection((s64_t)0, (s64_t)ub);
    }
    else
    {
        return IntegerIntervalProjection::top();
    }
}

/// Bitwise XOR of integer interval projections.
inline IntegerIntervalProjection operator^(const IntegerIntervalProjection& lhs,
                                           const IntegerIntervalProjection& rhs)
{
    auto next_power_of_2 = [](s64_t num) {
        int i = 1;
        while ((num >> i) != 0)
        {
            ++i;
        }
        return 1 << i;
    };
    if (lhs.isBottom() || rhs.isBottom())
        return IntegerIntervalProjection::bottom();
    else if (lhs.is_numeral() && rhs.is_numeral())
        return IntegerIntervalProjection(lhs.lb() ^ rhs.lb());
    else if (lhs.lb().getNumeral() >= 0 && !lhs.ub().is_infinity() &&
             rhs.lb().getNumeral() >= 0 && !rhs.ub().is_infinity())
    {
        s64_t m = std::max(lhs.ub().getNumeral(), rhs.ub().getNumeral());
        s64_t ub = next_power_of_2(s64_t(m)) - 1;
        return IntegerIntervalProjection((s64_t)0, (s64_t)ub);
    }
    else
    {
        return IntegerIntervalProjection::top();
    }
}

/// Write an IntegerIntervalProjection on a stream
inline std::ostream& operator<<(
    std::ostream& o, const IntegerIntervalProjection& IntegerIntervalProjection)
{
    IntegerIntervalProjection.dump(o);
    return o;
}

class EncodedAddressSet
{
    friend class RelExeState;

public:
    typedef Set<u32_t> AddrSet;

private:
    AddrSet _addrs;

    /// Return the internal index if idx is an address otherwise return the
    /// value of idx
    static inline u32_t getInternalID(u32_t idx)
    {
        return (idx & FlippedAddressMask);
    }

public:
    /// Default constructor
    EncodedAddressSet() {}

    /// Constructor
    EncodedAddressSet(const Set<u32_t>& addrs) : _addrs(addrs) {}

    EncodedAddressSet(u32_t addr) : _addrs({addr}) {}

    /// Default destructor
    ~EncodedAddressSet() = default;

    /// Copy constructor
    EncodedAddressSet(const EncodedAddressSet& other) : _addrs(other._addrs) {}

    /// Move constructor
    EncodedAddressSet(EncodedAddressSet&& other) noexcept
        : _addrs(std::move(other._addrs))
    {
    }

    /// Copy operator=
    EncodedAddressSet& operator=(const EncodedAddressSet& other)
    {
        if (!this->equals(other))
        {
            _addrs = other._addrs;
        }
        return *this;
    }

    /// Move operator=
    EncodedAddressSet& operator=(EncodedAddressSet&& other) noexcept
    {
        if (this != &other)
        {
            _addrs = std::move(other._addrs);
        }
        return *this;
    }

    bool equals(const EncodedAddressSet& rhs) const
    {
        return _addrs == rhs._addrs;
    }

    AddrSet::const_iterator begin() const
    {
        return _addrs.cbegin();
    }

    AddrSet::const_iterator end() const
    {
        return _addrs.cend();
    }

    bool empty() const
    {
        return _addrs.empty();
    }

    u32_t size() const
    {
        return _addrs.size();
    }

    std::pair<EncodedAddressSet::AddrSet::iterator, bool> insert(u32_t id)
    {
        return _addrs.insert(id);
    }

    const AddrSet& getVals() const
    {
        return _addrs;
    }

    void setVals(const AddrSet& vals)
    {
        _addrs = vals;
    }

    /// Current EncodedAddressSet joins with another EncodedAddressSet
    bool join_with(const EncodedAddressSet& other)
    {
        bool changed = false;
        for (const auto& addr : other)
        {
            if (!_addrs.count(addr))
            {
                if (insert(addr).second)
                    changed = true;
            }
        }
        return changed;
    }

    /// Return a intersected EncodedAddressSet
    bool meet_with(const EncodedAddressSet& other)
    {
        AddrSet s;
        for (const auto& id : other._addrs)
        {
            if (_addrs.find(id) != _addrs.end())
            {
                s.insert(id);
            }
        }
        bool changed = (_addrs != s);
        _addrs = std::move(s);
        return changed;
    }

    /// Return true if the EncodedAddressSet contains n
    bool contains(u32_t id) const
    {
        return _addrs.count(id);
    }

    bool hasIntersect(const EncodedAddressSet& other) const
    {
        for (const auto& addr : _addrs)
        {
            if (other._addrs.count(addr))
                return true;
        }
        return false;
    }

    inline bool isBottom() const
    {
        return empty();
    }

    const std::string toString() const
    {
        std::string str;
        std::stringstream rawStr(str);
        if (this->isBottom())
        {
            rawStr << "⊥";
        }
        else
        {
            rawStr << "[";
            for (auto it = _addrs.begin(), eit = _addrs.end(); it != eit; ++it)
            {
                rawStr << *it << ", ";
            }
            rawStr << "]";
        }
        return rawStr.str();
    }

    /// The physical address starts with 0x7f...... + idx
    static inline u32_t getVirtualMemAddress(u32_t idx)
    {
        // 0 is the null address, should not be used as a virtual address
        assert(idx != 0 && "idx can’t be 0 because it represents a nullptr");
        return AddressMask + idx;
    }

    /// Check bit value of val start with 0x7F000000, filter by 0xFF000000
    static inline bool isVirtualMemAddress(u32_t val)
    {
        return (val & 0xff000000) == AddressMask;
    }
};

class ScalarProjection
{

public:
    IntegerIntervalProjection interval;
    EncodedAddressSet addrs;

    ScalarProjection()
    {
        interval = IntegerIntervalProjection::bottom();
        addrs = EncodedAddressSet();
    }

    ScalarProjection(const ScalarProjection& other)
    {
        interval = other.interval;
        addrs = other.addrs;
    }

    inline bool isInterval() const
    {
        return !interval.isBottom();
    }
    inline bool isAddr() const
    {
        return !addrs.isBottom();
    }

    ScalarProjection(ScalarProjection&& other)
    {
        interval = SVFUtil::move(other.interval);
        addrs = SVFUtil::move(other.addrs);
    }

    // operator overload, supporting both interval and address
    ScalarProjection& operator=(const ScalarProjection& other)
    {
        interval = other.interval;
        addrs = other.addrs;
        return *this;
    }

    ScalarProjection& operator=(const ScalarProjection&& other)
    {
        interval = SVFUtil::move(other.interval);
        addrs = SVFUtil::move(other.addrs);
        return *this;
    }

    ScalarProjection& operator=(const IntegerIntervalProjection& other)
    {
        interval = other;
        addrs = EncodedAddressSet();
        return *this;
    }

    ScalarProjection& operator=(const EncodedAddressSet& other)
    {
        addrs = other;
        interval = IntegerIntervalProjection::bottom();
        return *this;
    }

    ScalarProjection(const IntegerIntervalProjection& ival)
        : interval(ival), addrs(EncodedAddressSet())
    {
    }

    ScalarProjection(const EncodedAddressSet& addr)
        : interval(IntegerIntervalProjection::bottom()), addrs(addr)
    {
    }

    IntegerIntervalProjection& getInterval()
    {
        return interval;
    }

    const IntegerIntervalProjection getInterval() const
    {
        return interval;
    }

    EncodedAddressSet& getAddrs()
    {
        return addrs;
    }

    const EncodedAddressSet getAddrs() const
    {
        return addrs;
    }

    ~ScalarProjection() {};

    bool equals(const ScalarProjection& rhs) const
    {
        return interval.equals(rhs.interval) && addrs.equals(rhs.addrs);
    }

    void join_with(const ScalarProjection& other)
    {
        interval.join_with(other.interval);
        addrs.join_with(other.addrs);
    }

    void meet_with(const ScalarProjection& other)
    {
        interval.meet_with(other.interval);
        addrs.meet_with(other.addrs);
    }

    void widen_with(const ScalarProjection& other)
    {
        interval.widen_with(other.interval);
        // TODO: widen Addrs
    }

    void narrow_with(const ScalarProjection& other)
    {
        interval.narrow_with(other.interval);
        // TODO: narrow Addrs
    }

    std::string toString() const
    {
        return "<" + interval.toString() + ", " + addrs.toString() + ">";
    }
};

} // namespace SVF

#endif // SVF_AE_SCALAR_PROJECTION_H
