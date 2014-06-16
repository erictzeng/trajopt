#include "trajopt/problem_description.hpp"
#include "trajopt/common.hpp"
#include <boost/foreach.hpp>
#include "utils/logging.hpp"
#include "sco/expr_ops.hpp"
#include "sco/expr_op_overloads.hpp"
#include "trajopt/kinematic_terms.hpp"
#include "trajopt/trajectory_costs.hpp"
#include "trajopt/tps_costs.hpp"
#include "trajopt/collision_terms.hpp"
#include "trajopt/rave_utils.hpp"
#include "trajopt/rave_utils.hpp"
#include "trajopt/traj_plotter.hpp"
#include "utils/eigen_conversions.hpp"
#include "utils/eigen_slicing.hpp"
#include <boost/algorithm/string.hpp>
#include "sco/optimizers.hpp"
using namespace Json;
using namespace std;
using namespace OpenRAVE;
using namespace trajopt;
using namespace util;

namespace {


bool gRegisteredMakers = false;



void ensure_only_members(const Value& v, const char** fields, int nvalid) {
  for (Json::ValueConstIterator it = v.begin(); it != v.end(); ++it) {
    bool valid = false;
    for (int j=0; j < nvalid; ++j) {
      if ( strcmp(it.memberName(), fields[j]) == 0) {
        valid = true;
        break;
      }
    }
    if (!valid) {
      PRINT_AND_THROW( boost::format("invalid field found: %s")%it.memberName());
    }
  } 
}


void RegisterMakers() {

  TermInfo::RegisterMaker("pose", &PoseCostInfo::create);
  TermInfo::RegisterMaker("joint_pos", &JointPosCostInfo::create);
  TermInfo::RegisterMaker("joint_vel", &JointVelCostInfo::create);
  TermInfo::RegisterMaker("collision", &CollisionCostInfo::create);
  TermInfo::RegisterMaker("rel_pts", &RelPtsCostInfo::create);
  TermInfo::RegisterMaker("tps", &TpsCostConstraintInfo::create);
  TermInfo::RegisterMaker("tps_pose", &TpsPoseCostInfo::create);

  TermInfo::RegisterMaker("joint", &JointConstraintInfo::create);
  TermInfo::RegisterMaker("cart_vel", &CartVelCntInfo::create);
  TermInfo::RegisterMaker("joint_vel_limits", &JointVelConstraintInfo::create);

  gRegisteredMakers = true;
}

RobotAndDOFPtr RADFromName(const string& name, RobotBasePtr robot) {
  if (name == "active") {
    return RobotAndDOFPtr(new RobotAndDOF(robot, robot->GetActiveDOFIndices(), robot->GetAffineDOF(), robot->GetAffineRotationAxis()));
  }
  vector<int> dof_inds;
  int affinedofs = 0;
  Vector rotationaxis(0,0,1);
  vector<string> components;
  boost::split(components, name, boost::is_any_of("+"));
  for (int i=0; i < components.size(); ++i) {
    std::string& component = components[i];
    if (RobotBase::ManipulatorPtr manip = GetManipulatorByName(*robot, component)) {
      vector<int> inds = manip->GetArmIndices();
      dof_inds.insert(dof_inds.end(), inds.begin(), inds.end());
    }
    else if (component == "base") {
      affinedofs |= DOF_X | DOF_Y | DOF_RotationAxis;
    }
    else if (KinBody::JointPtr joint = robot->GetJoint(component)) {
      dof_inds.push_back(joint->GetDOFIndex());
    }
    else PRINT_AND_THROW( boost::format("error in reading manip description: %s must be a manipulator, link, or 'base'")%component );
  }
  return RobotAndDOFPtr(new RobotAndDOF(robot, dof_inds, affinedofs, rotationaxis));
}


#if 0
BoolVec toMask(const VectorXd& x) {
  BoolVec out(x.size());
  for (int i=0; i < x.size(); ++i) out[i] = (x[i] > 0);
  return out;
}
#endif

bool allClose(const VectorXd& a, const VectorXd& b) {
  return (a-b).array().abs().maxCoeff() < 1e-4;
}

}

namespace Json { //funny thing with two-phase lookup

void fromJson(const Json::Value& v, Vector3d& x) {
  vector<double> vx;
  fromJsonArray(v, vx, 3);
  x = Vector3d(vx[0], vx[1], vx[2]);
}
void fromJson(const Json::Value& v, Vector4d& x) {
  vector<double> vx;
  fromJsonArray(v, vx, 4);
    x = Vector4d(vx[0], vx[1], vx[2], vx[3]);
}
void fromJson(const Json::Value& v, VectorXd& x) {
  vector<double> vx;
  fromJsonArray(v, vx);
  x = toVectorXd(vx);
}
void fromJson(const Json::Value& v, MatrixXd& x) {
  if (v.size() == 0) {
    x.resize(0, 0);
    return;
  }
  x.resize(v.size(), v[0].size());
  for (int i=0; i < v.size(); ++i) {
    assert(v[i].size() == v[0].size());
    DblVec row;
    fromJsonArray(v[i], row, v[i].size());
    x.row(i) = toVectorXd(row);
  }
}
}

namespace trajopt {

TRAJOPT_API ProblemConstructionInfo* gPCI;

void BasicInfo::fromJson(const Json::Value& v) {
  childFromJson(v, start_fixed, "start_fixed", true);
  childFromJson(v, n_steps, "n_steps");
  childFromJson(v, m_ext, "m_ext", 0);
  childFromJson(v, n_ext, "n_ext", 0);
  childFromJson(v, manip, "manip");
  childFromJson(v, robot, "robot", string(""));
  childFromJson(v, dofs_fixed, "dofs_fixed", IntVec());
  // TODO: optimization parameters, etc?
}


////
bool gReadingCosts=false, gReadingConstraints=false;
void fromJson(const Json::Value& v, TermInfoPtr& term) {
  string type;
  childFromJson(v, type, "type");
  LOG_DEBUG("reading term: %s", type.c_str());
  term = TermInfo::fromName(type);
  if (gReadingCosts) {
    if (!term) PRINT_AND_THROW( boost::format("failed to construct cost named %s")%type );    
    if (!dynamic_cast<MakesCost*>(term.get())) PRINT_AND_THROW( boost::format("%s is only a constraint, but you listed it as a cost")%type) ;
    term->term_type = TT_COST;
  }
  else if (gReadingConstraints) {
    if (!term) PRINT_AND_THROW( boost::format("failed to construct constraint named %s")%type );        
    if (!dynamic_cast<MakesConstraint*>(term.get())) PRINT_AND_THROW( boost::format("%s is only a cost, but you listed it as a constraint")%type);
    term->term_type = TT_CNT;
  }
  else assert(0 && "shouldnt happen");
  term->fromJson(v);
  childFromJson(v, term->name, "name", type);
}

std::map<string, TermInfo::MakerFunc> TermInfo::name2maker;
void TermInfo::RegisterMaker(const std::string& type, MakerFunc f) {
  name2maker[type] = f;
}

TermInfoPtr TermInfo::fromName(const string& type) {
  if (!gRegisteredMakers) RegisterMakers();
  if (name2maker.find(type) != name2maker.end()) {
    return (*name2maker[type])();
  }
  else {
    RAVELOG_ERROR("There is no cost of type %s\n", type.c_str());
    return TermInfoPtr();
  }
}

void InitInfo::fromJson(const Json::Value& v) {
  string type_str;
  childFromJson(v, type_str, "type");
  int n_steps = gPCI->basic_info.n_steps;
  int m_ext = gPCI->basic_info.m_ext;
  int n_ext = gPCI->basic_info.n_ext;
  int n_dof = gPCI->rad->GetDOF();

  if (type_str == "stationary") {
    data = toVectorXd(gPCI->rad->GetDOFValues()).transpose().replicate(n_steps, 1);
  }
  else if (type_str == "given_traj") {
    FAIL_IF_FALSE(v.isMember("data"));
    const Value& vdata = v["data"];
    if (vdata.size() != n_steps) {
      PRINT_AND_THROW("given initialization traj has wrong length");
    }
    data.resize(n_steps, n_dof);
    for (int i=0; i < n_steps; ++i) {
      DblVec row;
      fromJsonArray(vdata[i], row, n_dof);
      data.row(i) = toVectorXd(row);
    }
  }
  else if (type_str == "straight_line") {
    FAIL_IF_FALSE(v.isMember("endpoint"));
    DblVec endpoint;
    childFromJson(v, endpoint, "endpoint");
    if (endpoint.size() != n_dof) {
      PRINT_AND_THROW(boost::format("wrong number of dof values in initialization. expected %i got %j")%n_dof%endpoint.size());
    }
    data = TrajArray(n_steps, n_dof);
    DblVec start = gPCI->rad->GetDOFValues();
    for (int idof = 0; idof < n_dof; ++idof) {
      data.col(idof) = VectorXd::LinSpaced(n_steps, start[idof], endpoint[idof]);
    }
  }

  if (m_ext > 0 && n_ext > 0) {
    if (v.isMember("data_ext")) {
      const Value& vdata = v["data_ext"];
      if (vdata.size() != m_ext) {
        PRINT_AND_THROW("given initialization data ext has wrong length");
      }
        data_ext.resize(m_ext, n_ext);
      for (int i=0; i < m_ext; ++i) {
        DblVec row;
        fromJsonArray(vdata[i], row, n_ext);
        data_ext.row(i) = toVectorXd(row);
      }
    } else {
      data_ext = MatrixXd::Zero(m_ext, n_ext);
    }
  }
}

void ProblemConstructionInfo::fromJson(const Value& v) {
  childFromJson(v, basic_info, "basic_info");
  RobotBasePtr robot = (basic_info.robot=="") ? GetRobot(*env) : GetRobotByName(*env, basic_info.robot);
  if (!robot) {
    PRINT_AND_THROW("couldn't get robot");
  }
  rad = RADFromName(basic_info.manip, robot);
  if (!rad) {
    PRINT_AND_THROW( boost::format("couldn't get manip %s")%basic_info.manip );
  }

  gPCI = this;
  gReadingCosts=true;
  gReadingConstraints=false;
  if (v.isMember("costs")) fromJsonArray(v["costs"], cost_infos);
  gReadingCosts=false;
  gReadingConstraints=true;
  if (v.isMember("constraints")) fromJsonArray(v["constraints"], cnt_infos);
  gReadingConstraints=false;

  childFromJson(v, init_info, "init_info");
  gPCI = NULL;

}

TrajOptResult::TrajOptResult(OptResults& opt, TrajOptProb& prob) :
  cost_vals(opt.cost_vals),
  cnt_viols(opt.cnt_viols) {
  BOOST_FOREACH(const CostPtr& cost, prob.getCosts()) {
    cost_names.push_back(cost->name());
  }
  BOOST_FOREACH(const ConstraintPtr& cnt, prob.getConstraints()) {
    cnt_names.push_back(cnt->name());
  }
  traj = getTraj(opt.x, prob.GetVars());
  ext = getTraj(opt.x, prob.GetExtVars());
}

TrajOptResultPtr OptimizeProblem(TrajOptProbPtr prob, bool plot) {
  Configuration::SaverPtr saver = prob->GetRAD()->Save();
  BasicTrustRegionSQP opt(prob);
  opt.max_iter_ = 40;
  //opt.max_merit_coeff_increases_ = 10;
  opt.min_approx_improve_frac_ = .001;
  opt.improve_ratio_threshold_ = .2;
  opt.merit_error_coeff_ = 20;
  if (plot) {
    SetupPlotting(*prob, opt);
  }
  DblVec init_vars = trajToDblVec(prob->GetInitTraj());
  DblVec init_ext = trajToDblVec(prob->GetInitExt());
  init_vars.insert(init_vars.end(), init_ext.begin(), init_ext.end());
  opt.initialize(init_vars);
  opt.optimize();
  return TrajOptResultPtr(new TrajOptResult(opt.results(), *prob));
}

TrajOptProbPtr ConstructProblem(const ProblemConstructionInfo& pci) {

  const BasicInfo& bi = pci.basic_info;
  int n_steps = bi.n_steps;
  int m_ext = bi.m_ext;
  int n_ext = bi.n_ext;

  TrajOptProbPtr prob(new TrajOptProb(n_steps, pci.rad, m_ext, n_ext));
  int n_dof = prob->m_rad->GetDOF();

  DblVec cur_dofvals = prob->m_rad->GetDOFValues();

  if (bi.start_fixed) {
    if (pci.init_info.data.rows() > 0 && !allClose(toVectorXd(cur_dofvals), pci.init_info.data.row(0))) {
      PRINT_AND_THROW( "robot dof values don't match initialization. I don't know what you want me to use for the dof values");
    }
    for (int j=0; j < n_dof; ++j) {
      prob->addLinearConstraint(exprSub(AffExpr(prob->m_traj_vars(0,j)), cur_dofvals[j]), EQ);
    }
  }

  if (!bi.dofs_fixed.empty()) {
    BOOST_FOREACH(const int& dof_ind, bi.dofs_fixed) {
      for (int i=1; i < prob->GetNumSteps(); ++i) {
        prob->addLinearConstraint(exprSub(AffExpr(prob->m_traj_vars(i,dof_ind)), AffExpr(prob->m_traj_vars(0,dof_ind))), EQ);
      }
    }
  }

  BOOST_FOREACH(const TermInfoPtr& ci, pci.cost_infos) {
    ci->hatch(*prob);
  }
  BOOST_FOREACH(const TermInfoPtr& ci, pci.cnt_infos) {
    ci->hatch(*prob);
  }

  prob->SetInitTraj(pci.init_info.data);
  prob->SetInitExt(pci.init_info.data_ext);
  return prob;

}
TrajOptProbPtr ConstructProblem(const Json::Value& root, OpenRAVE::EnvironmentBasePtr env) {
  ProblemConstructionInfo pci(env);
  pci.fromJson(root);
  return ConstructProblem(pci);
}


TrajOptProb::TrajOptProb(int n_steps, ConfigurationPtr rad, int m_ext, int n_ext) : m_rad(rad) {
  DblVec lower, upper;
  m_rad->GetDOFLimits(lower, upper);
  int n_dof = m_rad->GetDOF();
  // put optimization joint limits a little inside robot joint limits
  // so numerical derivs work
  #if OPENRAVE_VERSION_MINOR <= 8
  for (int i=0; i < n_dof; ++i) lower[i] += 1e-4;
  for (int i=0; i < n_dof; ++i) upper[i] -= 1e-4;
  #endif

  vector<double> vlower, vupper;
  vector<string> names;
  for (int i=0; i < n_steps; ++i) {
    vlower.insert(vlower.end(), lower.data(), lower.data()+lower.size());
    vupper.insert(vupper.end(), upper.data(), upper.data()+upper.size());
    for (unsigned j=0; j < n_dof; ++j) {
      names.push_back( (boost::format("j_%i_%i")%i%j).str() );
    }
  }
  VarVector trajvarvec = createVariables(names, vlower, vupper);
  m_traj_vars = VarArray(n_steps, n_dof, trajvarvec.data());

  vector<string> ext_names;
  for (int i=0; i < m_ext; ++i) {
    for (int j=0; j < n_ext; ++j) {
      ext_names.push_back( (boost::format("e_%i_%i")%i%j).str() );
    }
  }
  VarVector extvarvec = createVariables(ext_names);
  m_ext_vars = VarArray(m_ext, n_ext, extvarvec.data());

  m_trajplotter.reset(new TrajPlotter(m_rad->GetEnv(), m_rad, m_traj_vars, m_ext_vars));

}


TrajOptProb::TrajOptProb() {
}

void SetupPlotting(TrajOptProb& prob, Optimizer& opt) {
  TrajPlotterPtr plotter = prob.GetPlotter();
  plotter->Add(prob.getCosts());
  plotter->Add(prob.getConstraints());
  opt.addCallback(boost::bind(&TrajPlotter::OptimizerCallback, *plotter, _1, _2));
}



void PoseCostInfo::fromJson(const Value& v) {
  FAIL_IF_FALSE(v.isMember("params"));
  const Value& params = v["params"];  
  childFromJson(params, timestep, "timestep", gPCI->basic_info.n_steps-1);
  childFromJson(params, xyz,"xyz");
  childFromJson(params, wxyz,"wxyz");
  childFromJson(params, pos_coeffs,"pos_coeffs", (Vector3d)Vector3d::Ones());
  childFromJson(params, rot_coeffs,"rot_coeffs", (Vector3d)Vector3d::Ones());

  string linkstr;
  childFromJson(params, linkstr, "link");
  link = GetLinkMaybeAttached(gPCI->rad->GetRobot(), linkstr);
  if (!link) {
    PRINT_AND_THROW(boost::format("invalid link name: %s")%linkstr);
  }

  const char* all_fields[] = {"timestep", "xyz", "wxyz", "pos_coeffs", "rot_coeffs","link"};
  ensure_only_members(params, all_fields, sizeof(all_fields)/sizeof(char*));

}

struct SqrtErrCalculator : public VectorOfVector {
  VectorOfVectorPtr f_;
  SqrtErrCalculator(VectorOfVectorPtr f) :
    f_(f) {}
  VectorXd operator()(const VectorXd& dof_vals) const {
    return f_->call(dof_vals).array().sqrt();
  }
};

struct l2NormErrCalculator : public VectorOfVector {
  VectorOfVectorPtr f_;
  VectorXd coeffs_;
  l2NormErrCalculator(VectorOfVectorPtr f, const VectorXd& coeffs) :
    f_(f),
    coeffs_(coeffs) {}
  VectorXd operator()(const VectorXd& dof_vals) const {
    VectorXd err = f_->call(dof_vals);
    if (coeffs_.size()>0) err.array() *= coeffs_.array();
    return err.norm() * VectorXd::Ones(1);
  }
};

void PoseCostInfo::hatch(TrajOptProb& prob) {
  VectorOfVectorPtr f(new CartPoseErrCalculator(toRaveTransform(wxyz, xyz), prob.GetRAD(), link));
  if (term_type == TT_COST) {
    VectorOfVectorPtr f_pos(new CartPoseErrCalculator(toRaveTransform(wxyz, xyz), prob.GetRAD(), link));
    VectorOfVectorPtr f_pos_norm(new l2NormErrCalculator(f_pos, concat(Vector3d::Zero(), pos_coeffs)));
    VectorOfVectorPtr f_rot(new CartPoseErrCalculator(toRaveTransform(wxyz, xyz), prob.GetRAD(), link));
    VectorOfVectorPtr f_rot_norm(new l2NormErrCalculator(f_rot, concat(rot_coeffs, Vector3d::Zero())));
    VectorOfVectorPtr f_pos_sqrt_norm(new SqrtErrCalculator(f_pos_norm));
    VectorOfVectorPtr f_rot_sqrt_norm(new SqrtErrCalculator(f_rot_norm));
    prob.addCost(CostPtr(new CostFromErrFunc(f_pos_sqrt_norm, prob.GetVarRow(timestep), VectorXd::Ones(1), SQUARED, name)));
    prob.addCost(CostPtr(new CostFromErrFunc(f_rot_sqrt_norm, prob.GetVarRow(timestep), VectorXd::Ones(1), SQUARED, name)));
  }
  else if (term_type == TT_CNT) {
    prob.addConstraint(ConstraintPtr(new ConstraintFromFunc(f, prob.GetVarRow(timestep), concat(rot_coeffs, pos_coeffs), EQ, name)));
  }

  prob.GetPlotter()->Add(PlotterPtr(new CartPoseErrorPlotter(f, prob.GetVarRow(timestep))));
  prob.GetPlotter()->AddLink(link);

}


void JointPosCostInfo::fromJson(const Value& v) {
  FAIL_IF_FALSE(v.isMember("params"));
  int n_steps = gPCI->basic_info.n_steps;
  const Value& params = v["params"];
  childFromJson(params, vals, "vals");
  childFromJson(params, coeffs, "coeffs");
  if (coeffs.size() == 1) coeffs = DblVec(n_steps, coeffs[0]);

  int n_dof = gPCI->rad->GetDOF();
  if (vals.size() != n_dof) {
    PRINT_AND_THROW( boost::format("wrong number of dof vals. expected %i got %i")%n_dof%vals.size());
  }
  childFromJson(params, timestep, "timestep", gPCI->basic_info.n_steps-1);
  
  const char* all_fields[] = {"vals", "coeffs", "timestep"};
  ensure_only_members(params, all_fields, sizeof(all_fields)/sizeof(char*));
  
  
}
void JointPosCostInfo::hatch(TrajOptProb& prob) {
  prob.addCost(CostPtr(new JointPosCost(prob.GetVarRow(timestep), toVectorXd(vals), toVectorXd(coeffs))));
  prob.getCosts().back()->setName(name);  
}


void CartVelCntInfo::fromJson(const Value& v) {
  FAIL_IF_FALSE(v.isMember("params"));
  const Value& params = v["params"];
  childFromJson(params, first_step, "first_step");
  childFromJson(params, last_step, "last_step");
  childFromJson(params, max_displacement,"max_displacement");

  FAIL_IF_FALSE((first_step >= 0) && (first_step <= gPCI->basic_info.n_steps-1) && (first_step < last_step));
  FAIL_IF_FALSE((last_step > 0) && (last_step <= gPCI->basic_info.n_steps-1));

  string linkstr;
  childFromJson(params, linkstr, "link");
  link = gPCI->rad->GetRobot()->GetLink(linkstr);
  if (!link) {
    PRINT_AND_THROW( boost::format("invalid link name: %s")%linkstr);
  }
  
  const char* all_fields[] = {"first_step", "last_step", "max_displacement","link"};
  ensure_only_members(params, all_fields, sizeof(all_fields)/sizeof(char*));
  
  
}

void CartVelCntInfo::hatch(TrajOptProb& prob) {
  for (int iStep = first_step; iStep < last_step; ++iStep) {
    prob.addConstraint(ConstraintPtr(new ConstraintFromFunc(
      VectorOfVectorPtr(new CartVelCalculator(prob.GetRAD(), link, max_displacement)),
       MatrixOfVectorPtr(new CartVelJacCalculator(prob.GetRAD(), link, max_displacement)), 
      concat(prob.GetVarRow(iStep), prob.GetVarRow(iStep+1)), VectorXd::Ones(0), INEQ, "CartVel")));     
  }
}

void JointVelCostInfo::fromJson(const Value& v) {
  FAIL_IF_FALSE(v.isMember("params"));
  const Value& params = v["params"];

  childFromJson(params, coeffs,"coeffs");
  int n_dof = gPCI->rad->GetDOF();
  if (coeffs.size() == 1) coeffs = DblVec(n_dof, coeffs[0]);
  else if (coeffs.size() != n_dof) {
    PRINT_AND_THROW( boost::format("wrong number of coeffs. expected %i got %i")%n_dof%coeffs.size());
  }
  
  const char* all_fields[] = {"coeffs"};
  ensure_only_members(params, all_fields, sizeof(all_fields)/sizeof(char*));
  
  
}

void JointVelCostInfo::hatch(TrajOptProb& prob) {
  prob.addCost(CostPtr(new JointVelCost(prob.GetVars(), toVectorXd(coeffs))));
  prob.getCosts().back()->setName(name);
}


void JointVelConstraintInfo::fromJson(const Value& v) {
  FAIL_IF_FALSE(v.isMember("params"));
  const Value& params = v["params"];
  
  int n_steps = gPCI->basic_info.n_steps;  
  int n_dof = gPCI->rad->GetDOF();  
  childFromJson(params, vals, "vals");
  childFromJson(params, first_step, "first_step", 0);
  childFromJson(params, last_step, "last_step", n_steps-1);
  FAIL_IF_FALSE(vals.size() == n_dof);
  FAIL_IF_FALSE((first_step >= 0) && (first_step < n_steps));
  FAIL_IF_FALSE((last_step >= first_step) && (last_step < n_steps));
  
  const char* all_fields[] = {"vals", "first_step", "last_step"};
  ensure_only_members(params, all_fields, sizeof(all_fields)/sizeof(char*));  
  
}
void JointVelConstraintInfo::hatch(TrajOptProb& prob) {
  for (int i = first_step; i <= last_step-1; ++i) {
    for (int j=0; j < vals.size(); ++j)  {
      AffExpr vel = prob.GetVar(i+1,j) -  prob.GetVar(i,j);
      prob.addLinearConstraint(vel - vals[j], INEQ);
      prob.addLinearConstraint(-vel - vals[j], INEQ);
    }
  }
}

void CollisionCostInfo::fromJson(const Value& v) {
  FAIL_IF_FALSE(v.isMember("params"));
  const Value& params = v["params"];

  int n_steps = gPCI->basic_info.n_steps;
  childFromJson(params, continuous, "continuous", true);
  childFromJson(params, first_step, "first_step", 0);
  childFromJson(params, last_step, "last_step", n_steps-1);
  childFromJson(params, gap, "gap", 1);
  FAIL_IF_FALSE( gap >= 0 );
  FAIL_IF_FALSE((first_step >= 0) && (first_step < n_steps));
  FAIL_IF_FALSE((last_step >= first_step) && (last_step < n_steps));
  childFromJson(params, coeffs, "coeffs");
  int n_terms = last_step - first_step + 1;
  if (coeffs.size() == 1) coeffs = DblVec(n_terms, coeffs[0]);
  else if (coeffs.size() != n_terms) {
    PRINT_AND_THROW (boost::format("wrong size: coeffs. expected %i got %i")%n_terms%coeffs.size());
  }
  childFromJson(params, dist_pen,"dist_pen");
  if (dist_pen.size() == 1) dist_pen = DblVec(n_terms, dist_pen[0]);
  else if (dist_pen.size() != n_terms) {
    PRINT_AND_THROW(boost::format("wrong size: dist_pen. expected %i got %i")%n_terms%dist_pen.size());
  }
  
  const char* all_fields[] = {"continuous", "first_step", "last_step", "gap", "coeffs", "dist_pen"};
  ensure_only_members(params, all_fields, sizeof(all_fields)/sizeof(char*));  
  
}
void CollisionCostInfo::hatch(TrajOptProb& prob) {
  if (term_type == TT_COST) {
    if (continuous) {
      for (int i=first_step; i <= last_step - gap; ++i) {
        prob.addCost(CostPtr(new CollisionCost(dist_pen[i-first_step], coeffs[i-first_step], prob.GetRAD(), prob.GetVarRow(i), prob.GetVarRow(i+gap))));
        prob.getCosts().back()->setName( (boost::format("%s_%i")%name%i).str() );
      }
    }
    else {
      for (int i=first_step; i <= last_step; ++i) {
        prob.addCost(CostPtr(new CollisionCost(dist_pen[i-first_step], coeffs[i-first_step], prob.GetRAD(), prob.GetVarRow(i))));
        prob.getCosts().back()->setName( (boost::format("%s_%i")%name%i).str() );
      }
    }
  }
  else { // ALMOST COPIED
    if (continuous) {
      for (int i=first_step; i < last_step; ++i) {
        prob.addIneqConstraint(ConstraintPtr(new CollisionConstraint(dist_pen[i-first_step], coeffs[i-first_step], prob.GetRAD(), prob.GetVarRow(i), prob.GetVarRow(i+1))));
        prob.getIneqConstraints().back()->setName( (boost::format("%s_%i")%name%i).str() );
      }
    }
    else {
      for (int i=first_step; i <= last_step; ++i) {
        prob.addIneqConstraint(ConstraintPtr(new CollisionConstraint(dist_pen[i-first_step], coeffs[i-first_step], prob.GetRAD(), prob.GetVarRow(i))));
        prob.getIneqConstraints().back()->setName( (boost::format("%s_%i")%name%i).str() );
      }
    }
  }



  CollisionCheckerPtr cc = CollisionChecker::GetOrCreate(*prob.GetEnv());
  cc->SetContactDistance(*std::max_element(dist_pen.begin(), dist_pen.end()) + .04);
}




void JointConstraintInfo::fromJson(const Value& v) {
  FAIL_IF_FALSE(v.isMember("params"));
  const Value& params = v["params"];
  childFromJson(params, vals, "vals");

  int n_dof = gPCI->rad->GetDOF();
  if (vals.size() != n_dof) {
    PRINT_AND_THROW( boost::format("wrong number of dof vals. expected %i got %i")%n_dof%vals.size());
  }
  childFromJson(params, timestep, "timestep", gPCI->basic_info.n_steps-1);
  
  const char* all_fields[] = {"vals", "timestep"};
  ensure_only_members(params, all_fields, sizeof(all_fields)/sizeof(char*));  
  
}

void JointConstraintInfo::hatch(TrajOptProb& prob) {
  VarVector vars = prob.GetVarRow(timestep);
  int n_dof = vars.size();
  for (int j=0; j < n_dof; ++j) {
    prob.addLinearConstraint(exprSub(AffExpr(vars[j]), vals[j]), EQ);    
  }
}


void RelPtsCostInfo::fromJson(const Value& v) {
  FAIL_IF_FALSE(v.isMember("params"));
  const Value& params = v["params"];
  childFromJson(params, timestep, "timestep", gPCI->basic_info.n_steps-1);
  childFromJson(params, xyzs, "xyzs");
  childFromJson(params, rel_xyzs, "rel_xyzs");
  childFromJson(params, pos_coeffs, "pos_coeffs", (VectorXd)VectorXd::Ones(xyzs.size()));

  if (xyzs.size() != rel_xyzs.size()) {
    PRINT_AND_THROW(boost::format("size of xyzs %d should be the same as size of rel_xyzs %d")%xyzs.size()%rel_xyzs.size());
  }

  string linkstr;
  childFromJson(params, linkstr, "link");
  link = GetLinkMaybeAttached(gPCI->rad->GetRobot(), linkstr);
  if (!link) {
    PRINT_AND_THROW(boost::format("invalid link name: %s")%linkstr);
  }

  const char* all_fields[] = {"timestep", "xyzs", "rel_xyzs", "pos_coeffs", "link"};
  ensure_only_members(params, all_fields, sizeof(all_fields)/sizeof(char*));

}

void RelPtsCostInfo::hatch(TrajOptProb& prob) {
  vector<OR::Vector> rave_xyzs(xyzs.size());
  vector<OR::Vector> rave_rel_xyzs(rel_xyzs.size());
  for (int i = 0; i < xyzs.size(); i++) {
    for (int j = 0; j < 3; j++) {
      rave_xyzs[i][j] = xyzs[i](j);
      rave_rel_xyzs[i][j] = rel_xyzs[i](j);
    }
  }
  VectorOfVectorPtr f(new RelPtsErrCalculator(rave_xyzs, rave_rel_xyzs, prob.GetRAD(), link));
  if (term_type == TT_COST) {
    VectorOfVectorPtr f_norm(new l2NormErrCalculator(f, pos_coeffs));
    VectorOfVectorPtr f_sqrt_norm(new SqrtErrCalculator(f_norm));
    prob.addCost(CostPtr(new CostFromErrFunc(f_sqrt_norm, prob.GetVarRow(timestep), VectorXd::Ones(1), SQUARED, name)));
  }
  else if (term_type == TT_CNT) {
    prob.addConstraint(ConstraintPtr(new ConstraintFromFunc(f, prob.GetVarRow(timestep), pos_coeffs, EQ, name)));
  }

  prob.GetPlotter()->Add(PlotterPtr(new RelPtsErrorPlotter(f, prob.GetVarRow(timestep))));
  prob.GetPlotter()->AddLink(link);

}


void TpsCostConstraintInfo::fromJson(const Value& v) {
  FAIL_IF_FALSE(v.isMember("params"));
  const Value& params = v["params"];

  FAIL_IF_FALSE(params.isMember("x_na"));
  Json::fromJson(params["x_na"], x_na);

  FAIL_IF_FALSE(params.isMember("y_ng"));
  Json::fromJson(params["y_ng"], y_ng);

  FAIL_IF_FALSE(params.isMember("bend_coefs"));
  Json::fromJson(params["bend_coefs"], bend_coefs);

  FAIL_IF_FALSE(params.isMember("rot_coefs"));
  Json::fromJson(params["rot_coefs"], rot_coefs);

  FAIL_IF_FALSE(params.isMember("wt_n"));
  Json::fromJson(params["wt_n"], wt_n);

  FAIL_IF_FALSE(params.isMember("N"));
  Json::fromJson(params["N"], N);

  childFromJson(params, alpha, "alpha");

  const char* all_fields[] = {"x_na", "y_ng", "bend_coefs", "rot_coefs", "wt_n", "N", "alpha"};
  ensure_only_members(params, all_fields, sizeof(all_fields)/sizeof(char*));
}

void TpsCostConstraintInfo::hatch(TrajOptProb& prob) {
  VarArray traj_vars = prob.GetVars();
  VarArray tps_vars = prob.GetExtVars();
  int n = tps_vars.rows();
  int dim = tps_vars.cols();
  assert(dim == 3);
  assert(x_na.rows() == n);
  assert(x_na.cols() == dim);
  assert(wt_n.rows() == n);
  assert(wt_n.cols() == dim);
  assert(N.rows() == n+dim+1);
  assert(N.cols() == n);

  boost::shared_ptr<TpsCost> tps_cost(new TpsCost(tps_vars, x_na, y_ng, bend_coefs, rot_coefs, wt_n, N, alpha));
  prob.addCost(CostPtr(tps_cost));
  prob.getCosts().back()->setName(name);

  prob.GetPlotter()->Add(PlotterPtr(new TpsCostPlotter(tps_cost)));
}

void TpsPoseCostInfo::fromJson(const Value& v) {
  FAIL_IF_FALSE(v.isMember("params"));
  const Value& params = v["params"];

  FAIL_IF_FALSE(params.isMember("x_na"));
  Json::fromJson(params["x_na"], x_na);

  FAIL_IF_FALSE(params.isMember("N"));
  Json::fromJson(params["N"], N);

  childFromJson(params, timestep, "timestep", gPCI->basic_info.n_steps-1);
  childFromJson(params, xyz,"xyz");
  childFromJson(params, wxyz,"wxyz");
  childFromJson(params, pos_coeffs,"pos_coeffs", (Vector3d)Vector3d::Ones());
  childFromJson(params, rot_coeffs,"rot_coeffs", (Vector3d)Vector3d::Ones());

  string linkstr;
  childFromJson(params, linkstr, "link");
  link = GetLinkMaybeAttached(gPCI->rad->GetRobot(), linkstr);
  if (!link) {
    PRINT_AND_THROW(boost::format("invalid link name: %s")%linkstr);
  }

  const char* all_fields[] = {"x_na", "N", "timestep", "xyz", "wxyz", "pos_coeffs", "rot_coeffs", "link"};
  ensure_only_members(params, all_fields, sizeof(all_fields)/sizeof(char*));
}

void TpsPoseCostInfo::hatch(TrajOptProb& prob) {
  VarArray traj_vars = prob.GetVars();
  VarArray tps_vars = prob.GetExtVars();
  VarVector dof_tps_vars = concat(traj_vars.row(timestep), tps_vars.flatten());
  VectorOfVectorPtr f(new TpsCartPoseErrCalculator(x_na, N, toRaveTransform(wxyz, xyz), prob.GetRAD(), link));
  prob.addCost(CostPtr(new CostFromErrFunc(f, dof_tps_vars, concat(rot_coeffs, pos_coeffs), ABS, name)));

  prob.GetPlotter()->Add(PlotterPtr(new TpsCartPoseErrorPlotter(f, dof_tps_vars)));
  prob.GetPlotter()->AddLink(link);
}

}
